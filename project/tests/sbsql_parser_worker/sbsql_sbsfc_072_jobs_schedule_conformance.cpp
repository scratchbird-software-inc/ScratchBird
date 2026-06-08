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
#include "lowering/lowering.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
using scratchbird::server::HostedDatabaseSnapshot;
using scratchbird::server::HostedDatabaseState;
using scratchbird::server::HostedEngineState;
using scratchbird::server::ServerSessionRecord;
using scratchbird::server::ServerSessionRegistry;
namespace agents = scratchbird::core::agents;
namespace sbps = scratchbird::server::sbps;

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

struct ExecuteResultForTest {
  std::string outcome;
  std::array<std::uint8_t, 16> request_uuid{};
  std::array<std::uint8_t, 16> cursor_uuid{};
  std::uint64_t row_count = 0;
  std::string operation_id;
  std::string row_packet;
  std::string detail;
};

struct ScheduleRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view canonical_family;
  std::string_view route_family;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view sql;
  std::string_view job_uuid;
  std::string_view schedule_uuid;
  bool requires_offset_list = false;
};

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
  for (const auto& value : values) {
    if (value == expected) return true;
  }
  return false;
}

std::uint16_t GetU16(const std::vector<std::uint8_t>& data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}

std::uint64_t GetU64(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::uint64_t value = 0;
  for (int byte = 7; byte >= 0; --byte) {
    value <<= 8u;
    value |= data[offset + static_cast<std::size_t>(byte)];
  }
  return value;
}

bool ReadString(const std::vector<std::uint8_t>& data,
                std::size_t* offset,
                std::string* out) {
  if (offset == nullptr || out == nullptr || *offset + 2 > data.size()) return false;
  std::size_t length = GetU16(data, *offset);
  *offset += 2;
  if (*offset + length > data.size()) return false;
  out->assign(reinterpret_cast<const char*>(data.data() + *offset), length);
  *offset += length;
  return true;
}

std::array<std::uint8_t, 16> GetUuid(const std::vector<std::uint8_t>& data,
                                     std::size_t offset) {
  std::array<std::uint8_t, 16> uuid{};
  std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(offset),
              uuid.size(),
              uuid.begin());
  return uuid;
}

ExecuteResultForTest DecodeExecuteResult(const std::vector<std::uint8_t>& payload) {
  ExecuteResultForTest result;
  std::size_t offset = 0;
  Require(ReadString(payload, &offset, &result.outcome), "SBSFC-072 execute outcome missing");
  Require(offset + 16 <= payload.size(), "SBSFC-072 execute request UUID missing");
  result.request_uuid = GetUuid(payload, offset);
  offset += 16;
  Require(offset + 16 <= payload.size(), "SBSFC-072 execute cursor UUID missing");
  result.cursor_uuid = GetUuid(payload, offset);
  offset += 16;
  Require(offset + 8 <= payload.size(), "SBSFC-072 execute row count missing");
  result.row_count = GetU64(payload, offset);
  offset += 8;
  Require(ReadString(payload, &offset, &result.operation_id),
          "SBSFC-072 execute operation id missing");
  Require(ReadString(payload, &offset, &result.row_packet),
          "SBSFC-072 execute row packet missing");
  Require(ReadString(payload, &offset, &result.detail),
          "SBSFC-072 execute detail missing");
  return result;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f7200-0000-7000-8000-000000000301";
  session.connection_uuid = "019f7200-0000-7000-8000-000000000302";
  session.database_uuid = "019f7200-0000-7000-8000-000000000303";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 72;
  session.security_policy_epoch = 73;
  session.descriptor_epoch = 74;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_072_jobs_schedule";
  config.parser_uuid = "019f7200-0000-7000-8000-000000000304";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-072-jobs-schedule";
  config.build_id = "sbsql-sbsfc-072-jobs-schedule";
  return config;
}

PipelineArtifacts RunPipeline(std::string_view sql) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(std::string(sql));
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session, {});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void PrintMessages(const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
}

void RequireCleanPipeline(const PipelineArtifacts& artifacts,
                          const ScheduleRow& row) {
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-072 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-072 AST failed");
  Require(artifacts.bound.bound, "SBSFC-072 bind failed");
  Require(artifacts.verifier.admitted, "SBSFC-072 SBLR verifier rejected");
  Require(artifacts.envelope.operation_family == row.route_family,
          "SBSFC-072 operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == row.route_family,
          "SBSFC-072 operation key mismatch");
  Require(artifacts.envelope.operation_id == row.operation_id,
          "SBSFC-072 operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == row.opcode,
          "SBSFC-072 opcode mismatch");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-072 allowed parser SQL execution");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.background_job_scheduler_required"),
          "SBSFC-072 missing scheduler authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.workload_quota_required"),
          "SBSFC-072 missing quota authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-072 missing no SQL execution authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SBSFC-072 missing no storage/finality authority");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.jobs.scheduler"),
          "SBSFC-072 missing scheduler descriptor");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.jobs.definition"),
          "SBSFC-072 missing jobs definition descriptor");
  Require(Contains(artifacts.envelope.payload, "\"jobs_scheduler_control\":true"),
          "SBSFC-072 missing jobs scheduler payload");
  Require(Contains(artifacts.envelope.payload, row.schedule_uuid),
          "SBSFC-072 missing stable schedule UUID");
  if (!row.job_uuid.empty()) {
    Require(Contains(artifacts.envelope.payload, row.job_uuid),
            "SBSFC-072 missing stable job UUID");
  }
  Require(Contains(artifacts.envelope.payload, "\"schedule_spec_present\":true"),
          "SBSFC-072 missing schedule spec marker");
  Require(Contains(artifacts.envelope.payload, "\"schedule_kind\":\"every\""),
          "SBSFC-072 missing schedule kind");
  Require(Contains(artifacts.envelope.payload, "\"schedule_expression\""),
          "SBSFC-072 missing schedule expression");
  Require(Contains(artifacts.envelope.payload, "\"job_lookup_scope\":\"database\""),
          "SBSFC-072 missing database lookup scope");
  Require(Contains(artifacts.envelope.payload,
                   "\"core_scheduler_api\":\"DatabaseLocalBackgroundJobScheduler\""),
          "SBSFC-072 missing scheduler API evidence");
  Require(Contains(artifacts.envelope.payload, "\"workload_quota_required\":true"),
          "SBSFC-072 missing quota payload evidence");
  Require(Contains(artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "SBSFC-072 missing no row storage marker");
  Require(Contains(artifacts.envelope.payload, "\"mga_finality_claimed\":false"),
          "SBSFC-072 missing no MGA finality marker");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "SBSFC-072 missing no SQL text marker");
  Require(Contains(artifacts.envelope.payload, row.surface_id),
          "SBSFC-072 payload missing row surface id");
  if (row.requires_offset_list) {
    Require(Contains(artifacts.envelope.payload, "\"schedule_offset_list_present\":true"),
            "SBSFC-072 missing schedule offset marker");
    Require(Contains(artifacts.envelope.payload, "SBSQL-0725D825F5F3"),
            "SBSFC-072 missing offset-list row id");
  }
  Require(!Contains(artifacts.envelope.payload, row.sql),
          "SBSFC-072 embedded source SQL text");
}

constexpr ScheduleRow kRows[] = {
    {"SBSQL-B6172AC78E5B", "alter_job_stmt", "sblr.catalog.mutation.v3",
     "sblr.catalog.mutation.v3", "jobs.scheduler.alter_job", "SBLR_JOBS_ALTER_JOB",
     "ALTER JOB route_job SCHEDULE EVERY 60;", "job:route_job", "schedule:route_job", false},
    {"SBSQL-F0AB7D8C3DF6", "create_schedule_stmt", "sblr.catalog.mutation.v3",
     "sblr.catalog.mutation.v3", "jobs.scheduler.create_schedule",
     "SBLR_JOBS_CREATE_SCHEDULE",
     "CREATE SCHEDULE route_sched FOR JOB scheduled_job EVERY 60 STARTS 120 ENDS 180;",
     "job:scheduled_job", "schedule:route_sched", true},
    {"SBSQL-BE0E4DA525D0", "alter_schedule_stmt", "sblr.catalog.mutation.v3",
     "sblr.catalog.mutation.v3", "jobs.scheduler.alter_schedule",
     "SBLR_JOBS_ALTER_SCHEDULE", "ALTER SCHEDULE route_sched EVERY 120;", "",
     "schedule:route_sched", false},
    {"SBSQL-0779CE111C6C", "schedule_spec", "sblr.jobs.operation.v3",
     "sblr.catalog.mutation.v3", "jobs.scheduler.create_schedule",
     "SBLR_JOBS_CREATE_SCHEDULE",
     "CREATE SCHEDULE route_sched FOR JOB scheduled_job EVERY 60 STARTS 120 ENDS 180;",
     "job:scheduled_job", "schedule:route_sched", true},
    {"SBSQL-01E1D3EF37D8", "event_schedule", "sblr.jobs.operation.v3",
     "sblr.catalog.mutation.v3", "jobs.scheduler.create_schedule",
     "SBLR_JOBS_CREATE_SCHEDULE",
     "CREATE SCHEDULE route_sched FOR JOB scheduled_job EVERY 60 STARTS 120 ENDS 180;",
     "job:scheduled_job", "schedule:route_sched", true},
    {"SBSQL-0725D825F5F3", "event_schedule_offset_list", "sblr.jobs.operation.v3",
     "sblr.catalog.mutation.v3", "jobs.scheduler.create_schedule",
     "SBLR_JOBS_CREATE_SCHEDULE",
     "CREATE SCHEDULE route_sched FOR JOB scheduled_job EVERY 60 STARTS 120 ENDS 180;",
     "job:scheduled_job", "schedule:route_sched", true},
};

void RequireRegistryEvidence() {
  for (const auto& row : kRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "SBSFC-072 generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "SBSFC-072 generated registry canonical name drifted");
    Require(registry_row->source_status == "native_now",
            "SBSFC-072 generated registry source status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "SBSFC-072 generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family == row.canonical_family,
            "SBSFC-072 generated registry SBLR family drifted");
  }
}

ServerSessionRegistry MakeRegistry(std::array<std::uint8_t, 16>* session_uuid) {
  ServerSessionRegistry registry;
  ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = "/tmp/sb_sbsfc_072_jobs_schedule.sbdb";
  session.database_uuid = "019f7200-0000-7000-8000-000000000401";
  session.catalog_generation = 72;
  session.security_epoch = 73;
  session.descriptor_epoch = 74;
  session.grant_epoch = 1;
  session.policy_generation = 1;
  session.name_resolution_epoch = 1;
  session.resource_epoch = 1;
  *session_uuid = session.session_uuid;
  registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session.session_uuid)] = session;
  return registry;
}

HostedEngineState MakeEngineState() {
  HostedEngineState state;
  state.engine_context_active = true;
  HostedDatabaseSnapshot database;
  database.state = HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path = "/tmp/sb_sbsfc_072_jobs_schedule.sbdb";
  database.database_uuid = "019f7200-0000-7000-8000-000000000401";
  state.databases.push_back(database);
  return state;
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::string& encoded) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(session_uuid, {}, encoded);
  return frame;
}

const agents::DatabaseLocalBackgroundJobScheduler& OnlyScheduler(
    const ServerSessionRegistry& registry) {
  Require(registry.job_schedulers_by_database_uuid.size() == 1,
          "SBSFC-072 expected one background job scheduler");
  return registry.job_schedulers_by_database_uuid.begin()->second;
}

ExecuteResultForTest ExecuteAccepted(ServerSessionRegistry* registry,
                                     const HostedEngineState& engine_state,
                                     const std::array<std::uint8_t, 16>& session_uuid,
                                     const PipelineArtifacts& artifacts) {
  const auto result = scratchbird::server::HandleExecuteSblr(
      registry, engine_state, ExecuteFrame(session_uuid, artifacts.envelope.payload));
  Require(result.accepted, "SBSFC-072 server route was not accepted");
  auto decoded = DecodeExecuteResult(result.payload);
  Require(decoded.outcome == "accepted", "SBSFC-072 execute outcome mismatch");
  Require(decoded.operation_id == artifacts.envelope.operation_id,
          "SBSFC-072 execute operation id mismatch");
  Require(decoded.row_count == 1, "SBSFC-072 execute row count mismatch");
  Require(Contains(decoded.row_packet, "\"status\":\"accepted\""),
          "SBSFC-072 execute row packet missing accepted status");
  Require(Contains(decoded.detail, "jobs_scheduler_route=database_local"),
          "SBSFC-072 execute detail missing scheduler route evidence");
  return decoded;
}

void RequireServerRoute() {
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid);
  const auto engine_state = MakeEngineState();

  const auto create_route_job = RunPipeline("CREATE JOB route_job;");
  const auto alter_job = RunPipeline("ALTER JOB route_job SCHEDULE EVERY 60;");
  const auto create_scheduled_job = RunPipeline("CREATE JOB scheduled_job;");
  const auto create_schedule = RunPipeline(
      "CREATE SCHEDULE route_sched FOR JOB scheduled_job EVERY 60 STARTS 120 ENDS 180;");
  const auto alter_schedule = RunPipeline("ALTER SCHEDULE route_sched EVERY 120;");

  (void)ExecuteAccepted(&registry, engine_state, session_uuid, create_route_job);
  (void)ExecuteAccepted(&registry, engine_state, session_uuid, alter_job);
  auto job = OnlyScheduler(registry).FindJob("job:route_job");
  Require(job.has_value(), "SBSFC-072 ALTER JOB lost the job record");
  Require(job->definition.schedule.has_value(),
          "SBSFC-072 ALTER JOB did not attach a schedule");
  Require(job->definition.schedule->schedule_uuid == "schedule:route_job",
          "SBSFC-072 ALTER JOB schedule UUID mismatch");
  Require(job->definition.schedule->expression == "60",
          "SBSFC-072 ALTER JOB schedule expression mismatch");
  Require(job->definition.schedule->interval_microseconds == 60000000u,
          "SBSFC-072 ALTER JOB interval mismatch");

  (void)ExecuteAccepted(&registry, engine_state, session_uuid, create_scheduled_job);
  (void)ExecuteAccepted(&registry, engine_state, session_uuid, create_schedule);
  job = OnlyScheduler(registry).FindJob("job:scheduled_job");
  Require(job.has_value(), "SBSFC-072 CREATE SCHEDULE lost the job record");
  Require(job->definition.schedule.has_value(),
          "SBSFC-072 CREATE SCHEDULE did not attach a schedule");
  Require(job->definition.schedule->schedule_uuid == "schedule:route_sched",
          "SBSFC-072 CREATE SCHEDULE UUID mismatch");
  Require(job->definition.schedule->starts_expression == "120",
          "SBSFC-072 CREATE SCHEDULE STARTS payload mismatch");
  Require(job->definition.schedule->ends_expression == "180",
          "SBSFC-072 CREATE SCHEDULE ENDS payload mismatch");

  (void)ExecuteAccepted(&registry, engine_state, session_uuid, alter_schedule);
  job = OnlyScheduler(registry).FindJob("job:scheduled_job");
  Require(job.has_value(), "SBSFC-072 ALTER SCHEDULE lost the job record");
  Require(job->definition.schedule.has_value(),
          "SBSFC-072 ALTER SCHEDULE did not retain a schedule");
  Require(job->definition.schedule->schedule_uuid == "schedule:route_sched",
          "SBSFC-072 ALTER SCHEDULE UUID mismatch");
  Require(job->definition.schedule->expression == "120",
          "SBSFC-072 ALTER SCHEDULE expression mismatch");
  Require(job->definition.schedule->interval_microseconds == 120000000u,
          "SBSFC-072 ALTER SCHEDULE interval mismatch");

  const auto missing = RunPipeline("ALTER SCHEDULE missing_sched EVERY 30;");
  const auto missing_result = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_uuid, missing.envelope.payload));
  Require(!missing_result.accepted,
          "SBSFC-072 missing schedule route unexpectedly succeeded");
  Require(!missing_result.diagnostics.empty(),
          "SBSFC-072 missing schedule route did not emit diagnostics");
  Require(missing_result.diagnostics.front().code == "BACKGROUND_JOBS.SCHEDULE_NOT_FOUND",
          "SBSFC-072 missing schedule route diagnostic mismatch");
}

}  // namespace

int main() {
  RequireRegistryEvidence();

  for (const auto& row : kRows) {
    const auto artifacts = RunPipeline(row.sql);
    RequireCleanPipeline(artifacts, row);
    Require(Contains(artifacts.envelope.payload, "SBSQL-0779CE111C6C"),
            "SBSFC-072 payload missing schedule_spec row");
    Require(Contains(artifacts.envelope.payload, "SBSQL-01E1D3EF37D8"),
            "SBSFC-072 payload missing event_schedule row");
  }

  RequireServerRoute();
  return EXIT_SUCCESS;
}
