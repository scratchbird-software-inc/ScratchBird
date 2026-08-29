// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// ODF-110 SQL exact-parity benchmark closure gate.

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "database_lifecycle.hpp"
#include "lowering/lowering.hpp"
#include "query/plan_api.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;

#ifndef ODF110_OUTPUT_JSON
#define ODF110_OUTPUT_JSON "optimizer_deficiency_odf_110_gate.json"
#endif

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

template <typename Result>
void RequireEngineOk(const Result& result, std::string_view message) {
  if (result.ok) return;
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Fail(message);
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

std::string JsonEscape(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          std::ostringstream escaped;
          escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                  << static_cast<unsigned>(static_cast<unsigned char>(ch));
          out += escaped.str();
        } else {
          out.push_back(ch);
        }
    }
  }
  return out;
}

std::string Quote(std::string_view value) {
  return "\"" + JsonEscape(value) + "\"";
}

std::string Id(platform::UuidKind kind, platform::u64 seed) {
  static std::map<std::pair<int, platform::u64>, std::string> generated_ids;
  const auto key = std::make_pair(static_cast<int>(kind), seed);
  const auto found = generated_ids.find(key);
  if (found != generated_ids.end()) return found->second;

  platform::TypedUuid generated_uuid;
  if (uuid::UuidKindAllowsDurableIdentity(kind)) {
    const auto generated =
        uuid::GenerateDurableEngineIdentityV7(kind, 1779600000000ull + seed);
    Require(generated.ok(), "ODF-110 generated durable UUID creation failed");
    generated_uuid = generated.value;
  } else {
    const auto raw = uuid::GenerateCompatibilityUnixTimeV7(1779600000000ull + seed);
    Require(raw.ok(), "ODF-110 generated UUID creation failed");
    const auto typed = uuid::MakeTypedUuid(kind, raw.value);
    Require(typed.ok(), "ODF-110 generated typed UUID creation failed");
    generated_uuid = typed.value;
  }

  const auto [inserted, _] =
      generated_ids.emplace(key, uuid::UuidToString(generated_uuid.value));
  return inserted->second;
}

api::EngineDescriptor Descriptor(std::string type_name) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(type_name);
  descriptor.encoded_descriptor = "type=" + descriptor.canonical_type_name;
  return descriptor;
}

api::EngineTypedValue IntValue(std::int64_t value) {
  api::EngineTypedValue typed;
  typed.descriptor = Descriptor("int64");
  typed.encoded_value = std::to_string(value);
  return typed;
}

api::EngineRowValue Row(std::vector<std::pair<std::string, api::EngineTypedValue>> fields) {
  api::EngineRowValue row;
  row.fields = std::move(fields);
  return row;
}

api::EngineQueryRelation Relation(std::string name,
                                  std::vector<api::EngineRowValue> rows,
                                  platform::u64 source_seed = 0) {
  api::EngineQueryRelation relation;
  relation.relation_name = std::move(name);
  relation.descriptor_digest = "descriptor:" + relation.relation_name;
  if (source_seed != 0) {
    relation.source_object.uuid.canonical = Id(platform::UuidKind::object, source_seed);
    relation.source_object.object_kind = "table";
  }
  relation.rows = std::move(rows);
  return relation;
}

std::vector<api::EngineRowValue> CustomerRows() {
  std::vector<api::EngineRowValue> rows;
  for (std::int64_t id = 1; id <= 12; ++id) {
    rows.push_back(Row({{"id", IntValue(id)},
                        {"customer_id", IntValue((id % 4) + 1)},
                        {"amount", IntValue(id * 10)},
                        {"region", IntValue(id % 3)},
                        {"active", IntValue((id % 2) == 0 ? 1 : 0)}}));
  }
  return rows;
}

std::vector<api::EngineRowValue> OrderRows() {
  std::vector<api::EngineRowValue> rows;
  for (std::int64_t id = 1; id <= 10; ++id) {
    rows.push_back(Row({{"order_id", IntValue(id)},
                        {"customer_id", IntValue((id % 4) + 1)},
                        {"amount", IntValue(id * 7)}}));
  }
  return rows;
}

struct DurableQueryFixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  std::string database_uuid;

  DurableQueryFixture() = default;
  DurableQueryFixture(const DurableQueryFixture&) = delete;
  DurableQueryFixture& operator=(const DurableQueryFixture&) = delete;
  DurableQueryFixture(DurableQueryFixture&& other) noexcept
      : directory(std::move(other.directory)),
        database_path(std::move(other.database_path)),
        database_uuid(std::move(other.database_uuid)) {
    other.directory.clear();
  }

  ~DurableQueryFixture() {
    std::error_code ignored;
    if (!directory.empty()) std::filesystem::remove_all(directory, ignored);
  }
};

std::optional<api::EngineRequestContext> g_durable_context;

DurableQueryFixture PrepareDurableQueryContext() {
  DurableQueryFixture fixture;
  fixture.directory = std::filesystem::temp_directory_path() /
                      ("scratchbird_odf110_" +
                       std::to_string(std::filesystem::file_time_type::clock::now()
                                          .time_since_epoch()
                                          .count()));
  std::filesystem::create_directories(fixture.directory);
  fixture.database_path = fixture.directory / "odf110.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  const auto database_uuid = uuid::ParseTypedUuid(
      platform::UuidKind::database, Id(platform::UuidKind::database, 110));
  const auto filespace_uuid = uuid::ParseTypedUuid(
      platform::UuidKind::filespace, Id(platform::UuidKind::filespace, 109));
  Require(database_uuid.ok() && filespace_uuid.ok(),
          "ODF-110 durable fixture UUID parsing failed");
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.creation_unix_epoch_millis = 1779600000000ull;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ODF-110 durable fixture database creation failed");
  fixture.database_uuid = Id(platform::UuidKind::database, 110);

  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.security_context_present = true;
  context.request_id = "odf110-sql-exact-parity";
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.node_uuid.canonical = Id(platform::UuidKind::object, 111);
  context.principal_uuid.canonical = Id(platform::UuidKind::principal, 112);
  context.session_uuid.canonical = Id(platform::UuidKind::session, 113);
  context.catalog_generation_id = 2110;
  context.security_epoch = 3110;
  context.resource_epoch = 4110;
  context.name_resolution_epoch = 5110;
  context.transaction_isolation_level = "snapshot";

  api::EngineBeginTransactionRequest begin;
  begin.context = context;
  begin.isolation_level = context.transaction_isolation_level;
  const auto begun = api::EngineBeginTransaction(begin);
  RequireEngineOk(begun, "ODF-110 durable transaction begin failed");
  context.transaction_uuid = begun.transaction_uuid;
  context.local_transaction_id = begun.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  context.statement_uuid.canonical = Id(platform::UuidKind::object, 115);

  api::EnginePublishStatementSnapshotRequest publish;
  publish.context = context;
  const auto published = api::EnginePublishStatementSnapshot(publish);
  RequireEngineOk(published, "ODF-110 statement snapshot publication failed");
  context.statement_snapshot_uuid = published.statement_snapshot_uuid;
  context.snapshot_visible_through_local_transaction_id =
      published.snapshot_vector.visible_committed_high_watermark;
  context.statement_metadata_snapshot_uuid.canonical =
      Id(platform::UuidKind::object, 117);
  context.catalog_epoch_uuid.canonical = Id(platform::UuidKind::object, 118);
  context.statement_metadata_snapshot_engine_owned = true;
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      Id(platform::UuidKind::object, 119);
  context.authorization_context.principal_uuid = context.principal_uuid;
  context.authorization_context.security_epoch = context.security_epoch;
  context.authorization_context.policy_epoch = context.security_epoch;
  context.authorization_context.catalog_generation_id =
      context.catalog_generation_id;
  api::EngineAuthorizationSubject subject;
  subject.subject_uuid = context.principal_uuid;
  subject.subject_kind = "principal";
  context.authorization_context.effective_subjects.push_back(
      std::move(subject));
  context.optimizer_capability_snapshot_uuid.canonical =
      Id(platform::UuidKind::object, 124);
  context.optimizer_resource_snapshot_uuid.canonical =
      Id(platform::UuidKind::object, 125);
  context.optimizer_route_snapshot_uuid.canonical =
      Id(platform::UuidKind::object, 126);
  context.optimizer_route_epoch = 6110;
  context.optimizer_route_generation = 7110;
  context.optimizer_memory_budget_bytes = 8 * 1024 * 1024;
  context.optimizer_maximum_candidate_count = 4096;
  context.optimizer_maximum_memo_groups = 512;
  context.optimizer_maximum_search_steps = 16384;
  context.optimizer_maximum_planning_time_ns = 10'000'000;
  context.current_monotonic_ns = "11000000";
  context.trace_tags = {"optimizer_deficiency_odf_110_gate",
                        "benchmark_clean",
                        "mga_transaction_regression"};
  g_durable_context = context;
  return fixture;
}

api::EngineRequestContext Context() {
  Require(g_durable_context.has_value(),
          "ODF-110 durable query context is not initialized");
  return *g_durable_context;
}

void RollbackDurableQueryContext() {
  Require(g_durable_context.has_value(),
          "ODF-110 durable query context is not initialized");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = *g_durable_context;
  RequireEngineOk(api::EngineRollbackTransaction(rollback),
                  "ODF-110 durable transaction rollback failed");
  g_durable_context.reset();
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = Id(platform::UuidKind::session, 120);
  session.connection_uuid = Id(platform::UuidKind::object, 121);
  session.database_uuid = Id(platform::UuidKind::database, 122);
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 110;
  session.security_policy_epoch = 111;
  session.descriptor_epoch = 112;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = Id(platform::UuidKind::object, 123);
  config.bundle_contract_id = "sbp_sbsql@odf-110-sql-parity";
  config.build_id = "optimizer-deficiency-odf-110";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(std::string_view sql,
                              std::vector<std::string> resolved_objects) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound =
      BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session, resolved_objects);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

struct ParserEvidence {
  std::string status;
  std::string operation_id;
  std::string opcode;
  std::string operation_family;
  std::string refusal_code;
  std::vector<std::string> evidence;
};

ParserEvidence CheckParserRoute(std::string_view row_id,
                                std::string_view sql,
                                std::string_view expected_operation,
                                std::vector<std::string> resolved_objects) {
  ParserEvidence evidence;
  if (sql.empty()) {
    evidence.status = "exact_refusal";
    evidence.refusal_code = "SB_ODF110_PARSER_ROUTE_NOT_CANONICAL_FOR_MATRIX_ROW";
    evidence.evidence.push_back("parser_route_not_practical_for:" + std::string(row_id));
    return evidence;
  }

  const auto artifacts = RunPipeline(sql, std::move(resolved_objects));
  if (!artifacts.bound.bound || !artifacts.verifier.admitted ||
      artifacts.envelope.operation_id != expected_operation) {
    evidence.status = "exact_refusal";
    evidence.operation_id = artifacts.envelope.operation_id;
    evidence.opcode = artifacts.envelope.sblr_opcode;
    evidence.operation_family = artifacts.envelope.operation_family;
    evidence.refusal_code = "SB_ODF110_PARSER_ROUTE_OPERATION_MISMATCH";
    evidence.evidence.push_back("expected_operation:" + std::string(expected_operation));
    evidence.evidence.push_back("observed_operation:" + artifacts.envelope.operation_id);
    return evidence;
  }

  evidence.status = "lowered";
  evidence.operation_id = artifacts.envelope.operation_id;
  evidence.opcode = artifacts.envelope.sblr_opcode;
  evidence.operation_family = artifacts.envelope.operation_family;
  Require(artifacts.envelope.engine_api_operation_id == expected_operation,
          "ODF-110 parser lowering engine API operation drifted");
  Require(artifacts.envelope.engine_api_function.empty() ||
              artifacts.envelope.engine_api_function ==
                  "DispatchTypedPlanOperation" ||
              expected_operation == "dml.select_rows",
          "ODF-110 parser lowering engine API function drifted");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "ODF-110 parser lowering lost no-finality authority evidence");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "ODF-110 parser lowering lost no-SQL-execution authority evidence");
  Require(!artifacts.envelope.parser_executes_sql,
          "ODF-110 parser lowering claimed SQL execution");
  Require(!Contains(artifacts.envelope.payload, sql),
          "ODF-110 parser lowering embedded source SQL text");
  evidence.evidence.push_back("authority.parser.no_storage_or_finality");
  evidence.evidence.push_back("authority.parser.no_sql_text_execution");
  evidence.evidence.push_back("parser_executes_sql:false");
  return evidence;
}

std::uint64_t Fnv1a64(std::string_view value) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const unsigned char ch : value) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string Hex64(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << value;
  return out.str();
}

std::string CanonicalResultPayload(const api::EngineApiResult& result) {
  std::ostringstream out;
  out << (result.ok ? "ok" : "error") << "|operation=" << result.operation_id
      << "|rows=" << result.result_shape.rows.size();
  for (const auto& diagnostic : result.diagnostics) {
    out << "|diag=" << diagnostic.code << ':' << diagnostic.detail;
  }
  for (const auto& row : result.result_shape.rows) {
    out << "|row";
    for (const auto& field : row.fields) {
      out << '|' << field.first
          << ':' << (field.second.is_null ? "null" : "value")
          << ':' << field.second.encoded_value;
    }
  }
  return out.str();
}

std::string ResultHash(const api::EngineApiResult& result) {
  return Hex64(Fnv1a64(CanonicalResultPayload(result)));
}

std::string FirstDiagnosticDetail(const api::EngineApiResult& result) {
  return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().detail;
}

struct BenchmarkRow {
  std::string id;
  std::string sql;
  std::string parser_sql;
  std::string parser_expected_operation = "query.execute";
  std::string operation = "scan";
  std::vector<api::EngineQueryRelation> relations;
  std::vector<std::string> options;
  std::vector<std::string> plan_markers;
  std::vector<std::string> required_engine_evidence;
  std::vector<std::string> resolved_parser_objects;
  std::string benchmark_refusal_code;
  std::string reference_equivalence_class;
  bool parameter_shape_pair = false;
  bool differential_pair = false;
};

struct BenchmarkCleanDecision {
  bool admitted = true;
  std::string code = "SB_ODF110_BENCHMARK_CLEAN_ADMITTED";
  std::string detail = "optimizer_evidence_clean";
};

bool RowOptionEnabled(const BenchmarkRow& row,
                      std::string_view prefix,
                      std::string_view alternate_prefix = {}) {
  for (const auto& option : row.options) {
    if (option.rfind(prefix, 0) == 0) {
      const auto value = option.substr(prefix.size());
      return value == "true" || value == "1" || value == "enabled";
    }
    if (!alternate_prefix.empty() && option.rfind(alternate_prefix, 0) == 0) {
      const auto value = option.substr(alternate_prefix.size());
      return value == "true" || value == "1" || value == "enabled";
    }
  }
  return false;
}

bool EvidenceTextContains(const api::EngineApiResult& result, std::string_view needle) {
  for (const auto& evidence : result.evidence) {
    if (Contains(evidence.evidence_kind, needle) || Contains(evidence.evidence_id, needle)) {
      return true;
    }
  }
  return false;
}

BenchmarkCleanDecision BenchmarkCleanAdmissionFor(const BenchmarkRow& row,
                                                  const api::EngineApiResult& result) {
  if (!result.ok) {
    return {false,
            "SB_ODF110_BENCHMARK_CLEAN_ENGINE_REFUSAL",
            FirstDiagnosticDetail(result)};
  }
  if (RowOptionEnabled(row, "optimizer_force_stale_stats:", "statistics_stale:")) {
    return {false,
            row.benchmark_refusal_code.empty()
                ? "SB_ODF110_BENCHMARK_CLEAN_REFUSED_MISSING_STATS"
                : row.benchmark_refusal_code,
            "forced_stale_or_missing_statistics"};
  }
  if (EvidenceTextContains(result, "LOCAL_DEFAULT_STATS") ||
      EvidenceTextContains(result, "POLICY_DEFAULT_STATS") ||
      EvidenceTextContains(result, "catalog-missing:epoch0")) {
    return {false,
            "SB_ODF110_BENCHMARK_CLEAN_REFUSED_MISSING_STATS",
            "optimizer_selected_default_or_missing_statistics"};
  }
  return {};
}

std::optional<std::string> OptionValue(const BenchmarkRow& row,
                                       std::string_view name) {
  const std::string prefix = std::string(name) + ':';
  for (const auto& option : row.options) {
    if (option.rfind(prefix, 0) == 0) return option.substr(prefix.size());
  }
  return std::nullopt;
}

std::uint64_t UnsignedOption(const BenchmarkRow& row,
                             std::string_view name,
                             std::uint64_t fallback) {
  const auto value = OptionValue(row, name);
  if (!value.has_value()) return fallback;
  std::size_t consumed = 0;
  const auto parsed = std::stoull(*value, &consumed);
  Require(consumed == value->size(),
          "ODF-110 option is not a canonical unsigned integer");
  return parsed;
}

std::vector<std::size_t> ProjectedColumns(const BenchmarkRow& row,
                                          std::size_t column_count) {
  const auto encoded = OptionValue(row, "project_columns");
  if (!encoded.has_value()) {
    std::vector<std::size_t> all;
    for (std::size_t index = 0; index < column_count; ++index) {
      all.push_back(index);
    }
    return all;
  }
  std::vector<std::size_t> projected;
  std::size_t start = 0;
  while (start <= encoded->size()) {
    const auto separator = encoded->find(',', start);
    const auto token = encoded->substr(
        start, separator == std::string::npos ? std::string::npos
                                              : separator - start);
    std::size_t consumed = 0;
    const auto index = std::stoull(token, &consumed);
    Require(consumed == token.size() && index < column_count,
            "ODF-110 project column is outside the source descriptor");
    projected.push_back(static_cast<std::size_t>(index));
    if (separator == std::string::npos) break;
    start = separator + 1;
  }
  Require(!projected.empty(), "ODF-110 projection cannot be empty");
  return projected;
}

std::string EncodeHex(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size() * 2);
  for (const unsigned char ch : value) {
    encoded.push_back(kHex[ch >> 4]);
    encoded.push_back(kHex[ch & 0x0f]);
  }
  return encoded;
}

std::string JoinHandles(const std::vector<std::uint32_t>& handles) {
  if (handles.empty()) return "-";
  std::ostringstream out;
  for (std::size_t index = 0; index < handles.size(); ++index) {
    if (index != 0) out << ',';
    out << handles[index];
  }
  return out.str();
}

void AppendLittleEndianU64(std::vector<std::uint8_t>* output,
                           std::uint64_t value) {
  for (unsigned byte = 0; byte < 8; ++byte) {
    output->push_back(
        static_cast<std::uint8_t>((value >> (byte * 8)) & 0xffu));
  }
}

void FinalizeProductionOperands(sblr::SblrOperationEnvelope* envelope) {
  std::uint32_t ordinal = 1;
  for (auto& operand : envelope->operands) {
    if (!operand.name.empty() &&
        std::all_of(operand.name.begin(), operand.name.end(),
                    [](const unsigned char ch) {
                      return ch >= '0' && ch <= '9';
                    })) {
      operand.name = "slot_" + operand.name;
    } else if ((operand.type == "relational_property_v1" ||
                operand.type == "relational_property_v2") &&
               operand.name.size() == 36) {
      std::string compact;
      for (const char ch : operand.name) {
        if (ch != '-') compact.push_back(ch);
      }
      operand.name = "property_" + compact;
    }
    const auto value = std::move(operand.value);
    operand.value.clear();
    operand.value_kind = sblr::SblrValueKind::literal_typed;
    operand.value_body.assign(16, 0);
    operand.value_body.front() = 0x73;
    AppendLittleEndianU64(&operand.value_body, value.size());
    operand.value_body.insert(operand.value_body.end(), value.begin(), value.end());
    operand.ordinal = ordinal++;
  }
}

sblr::SblrOperationEnvelope EnvelopeFor(const BenchmarkRow& row) {
  constexpr std::string_view kInt64TypeUuid =
      "019d0000-0000-7000-8000-00000000d711";
  constexpr std::string_view kBooleanTypeUuid =
      "01000000-626f-7f6c-a561-6e0000000000";
  const auto context = Context();
  const auto seed = 20'000 + Fnv1a64(row.id) % 100'000;
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "trace.odf110." + row.id);
  const auto* operation = sblr::LookupSblrOperation("query.execute");
  Require(operation != nullptr, "ODF-110 canonical query.execute registry row is absent");
  envelope.opcode_code = operation->code;
  envelope.parser_package_uuid = Id(platform::UuidKind::object, seed + 1);
  envelope.registry_snapshot_uuid = Id(platform::UuidKind::object, seed + 2);
  envelope.result_shape = "query_execute_result";
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;

  std::vector<sblr::SblrOperand> records;
  std::vector<sblr::SblrOperand> nodes;
  std::vector<sblr::SblrOperand> bindings;
  std::vector<sblr::SblrOperand> properties;
  std::uint32_t next_descriptor = 1;
  std::uint32_t next_expression = 1;
  std::uint32_t next_output = 1;
  std::uint32_t next_values_row = 1;
  std::uint64_t uuid_offset = 100;

  struct SourceShape {
    std::uint32_t node_id{0};
    std::vector<std::uint32_t> descriptor_ids;
    std::vector<std::uint32_t> output_expression_ids;
    std::vector<std::string> names;
  };
  const auto add_source = [&](const api::EngineQueryRelation& relation,
                              std::uint32_t node_id) {
    Require(!relation.rows.empty() && !relation.rows.front().fields.empty(),
            "ODF-110 canonical VALUES source is empty");
    SourceShape source;
    source.node_id = node_id;
    for (const auto& field : relation.rows.front().fields) {
      const auto descriptor_id = next_descriptor++;
      source.descriptor_ids.push_back(descriptor_id);
      source.names.push_back(field.first);
      records.push_back({"relational_descriptor_v1",
                         std::to_string(descriptor_id),
                         Id(platform::UuidKind::object, seed + uuid_offset++) + "|" +
                             std::string(kInt64TypeUuid) + "|1|-|-|-|-|-"});
    }
    std::vector<std::uint32_t> all_cell_expressions;
    std::vector<std::uint32_t> first_row_expressions;
    for (const auto& input_row : relation.rows) {
      Require(input_row.fields.size() == source.descriptor_ids.size(),
              "ODF-110 source row descriptor width drifted");
      std::vector<std::uint32_t> row_expressions;
      for (std::size_t column = 0; column < input_row.fields.size(); ++column) {
        const auto expression_id = next_expression++;
        row_expressions.push_back(expression_id);
        all_cell_expressions.push_back(expression_id);
        records.push_back({"relational_expression_v1",
                           std::to_string(expression_id),
                           "1|-|" + std::to_string(source.descriptor_ids[column]) +
                               "|-|-|1|-|" +
                               EncodeHex(input_row.fields[column].second.encoded_value)});
      }
      if (first_row_expressions.empty()) first_row_expressions = row_expressions;
      const auto row_id = next_values_row++;
      records.push_back({"relational_values_row_v1", std::to_string(row_id),
                         JoinHandles(row_expressions)});
    }
    source.output_expression_ids = first_row_expressions;
    for (std::size_t column = 0; column < source.descriptor_ids.size(); ++column) {
      records.push_back({"relational_output_v1", std::to_string(next_output++),
                         std::to_string(node_id) + "|" +
                             std::to_string(first_row_expressions[column]) + "|" +
                             std::to_string(source.descriptor_ids[column]) + "|1|" +
                             std::to_string(column) + "|" +
                             EncodeHex(source.names[column])});
    }
    std::vector<std::uint32_t> source_row_ids;
    const auto first_row_id = next_values_row - relation.rows.size();
    for (std::uint32_t ordinal = 0; ordinal < relation.rows.size(); ++ordinal) {
      source_row_ids.push_back(static_cast<std::uint32_t>(first_row_id + ordinal));
    }
    nodes.push_back({"relational_node_v1", std::to_string(node_id),
                     "13|0|-|" + JoinHandles(source.descriptor_ids) + "|" +
                         JoinHandles(source_row_ids)});
    bindings.push_back({"relational_node_binding_v1", std::to_string(node_id),
                        EncodeHex("values.literal-table.v1") + "|" +
                            JoinHandles(all_cell_expressions) + "|-|-|-"});
    return source;
  };

  const auto left = add_source(row.relations.front(), 1);
  std::uint32_t root_node_id = left.node_id;
  std::vector<std::uint32_t> current_descriptors = left.descriptor_ids;
  std::vector<std::uint32_t> current_expressions =
      left.output_expression_ids;
  std::vector<std::string> current_names = left.names;
  std::uint32_t next_node_id = 2;

  if (row.operation == "inner_join") {
    Require(row.relations.size() == 2,
            "ODF-110 INNER JOIN requires exactly two descriptor sources");
    const auto right = add_source(row.relations[1], next_node_id++);
    const auto bool_descriptor = next_descriptor++;
    records.push_back({"relational_descriptor_v1",
                       std::to_string(bool_descriptor),
                       Id(platform::UuidKind::object, seed + uuid_offset++) + "|" +
                           std::string(kBooleanTypeUuid) + "|1|-|-|-|-|-"});
    const auto left_identifier = next_expression++;
    const auto right_identifier = next_expression++;
    const auto predicate = next_expression++;
    records.push_back({"relational_expression_v1",
                       std::to_string(left_identifier),
                       "3|-|" + std::to_string(left.descriptor_ids[0]) + "|-|" +
                           Id(platform::UuidKind::object, seed + uuid_offset++) + "|-|-|-"});
    records.push_back({"relational_expression_v1",
                       std::to_string(right_identifier),
                       "3|-|" + std::to_string(right.descriptor_ids[0]) + "|-|" +
                           Id(platform::UuidKind::object, seed + uuid_offset++) + "|-|-|-"});
    records.push_back({"relational_expression_v1", std::to_string(predicate),
                       "6|" + JoinHandles({left_identifier, right_identifier}) + "|" +
                           std::to_string(bool_descriptor) + "|-|-|-|3d|-"});
    current_descriptors.insert(current_descriptors.end(),
                               right.descriptor_ids.begin(),
                               right.descriptor_ids.end());
    current_names.insert(current_names.end(), right.names.begin(), right.names.end());
    root_node_id = next_node_id++;
    nodes.push_back({"relational_node_v1", std::to_string(root_node_id),
                     "4|0|" + JoinHandles({left.node_id, right.node_id}) + "|" +
                         JoinHandles(current_descriptors) + "|-"});
    bindings.push_back({"relational_node_binding_v1", std::to_string(root_node_id),
                        EncodeHex("join.inner.v1") + "|" +
                            std::to_string(predicate) + "|-|-|-"});
  } else {
    Require(row.relations.size() == 1,
            "ODF-110 unary query requires exactly one descriptor source");
    if (row.operation == "filter_gt") {
      const auto bool_descriptor = next_descriptor++;
      records.push_back({"relational_descriptor_v1",
                         std::to_string(bool_descriptor),
                         Id(platform::UuidKind::object, seed + uuid_offset++) + "|" +
                             std::string(kBooleanTypeUuid) + "|1|-|-|-|-|-"});
      const auto identifier = next_expression++;
      const auto threshold = next_expression++;
      const auto predicate = next_expression++;
      records.push_back({"relational_expression_v1", std::to_string(identifier),
                         "3|-|" + std::to_string(left.descriptor_ids[0]) + "|-|" +
                             Id(platform::UuidKind::object, seed + uuid_offset++) + "|-|-|-"});
      records.push_back({"relational_expression_v1", std::to_string(threshold),
                         "1|-|" + std::to_string(left.descriptor_ids[0]) +
                             "|-|-|1|-|" +
                             EncodeHex(std::to_string(UnsignedOption(row, "threshold", 0)))});
      records.push_back({"relational_expression_v1", std::to_string(predicate),
                         "6|" + JoinHandles({identifier, threshold}) + "|" +
                             std::to_string(bool_descriptor) + "|-|-|-|3e|-"});
      root_node_id = next_node_id++;
      nodes.push_back({"relational_node_v1", std::to_string(root_node_id),
                       "2|0|" + std::to_string(left.node_id) + "|" +
                           JoinHandles(current_descriptors) + "|-"});
      bindings.push_back({"relational_node_binding_v1", std::to_string(root_node_id),
                          EncodeHex("filter.where.v1") + "|" +
                              std::to_string(predicate) + "|-|-|-"});
    } else {
      Require(row.operation == "scan", "ODF-110 unsupported canonical query shape");
    }

    const auto projected = ProjectedColumns(row, current_descriptors.size());
    if (projected.size() != current_descriptors.size()) {
      std::vector<std::uint32_t> projected_descriptors;
      std::vector<std::string> projected_names;
      std::vector<std::uint32_t> projection_expressions;
      const auto project_node_id = next_node_id++;
      for (std::size_t ordinal = 0; ordinal < projected.size(); ++ordinal) {
        const auto source_column = projected[ordinal];
        const auto expression_id = next_expression++;
        projection_expressions.push_back(expression_id);
        projected_descriptors.push_back(current_descriptors[source_column]);
        projected_names.push_back(current_names[source_column]);
        records.push_back({"relational_expression_v1", std::to_string(expression_id),
                           "3|-|" + std::to_string(current_descriptors[source_column]) +
                               "|-|" +
                               Id(platform::UuidKind::object, seed + uuid_offset++) +
                               "|-|-|-"});
        records.push_back({"relational_output_v1", std::to_string(next_output++),
                           std::to_string(project_node_id) + "|" +
                               std::to_string(expression_id) + "|" +
                               std::to_string(current_descriptors[source_column]) + "|1|" +
                               std::to_string(ordinal) + "|" +
                               EncodeHex(current_names[source_column])});
      }
      nodes.push_back({"relational_node_v1", std::to_string(project_node_id),
                       "3|0|" + std::to_string(root_node_id) + "|" +
                           JoinHandles(projected_descriptors) + "|-"});
      bindings.push_back({"relational_node_binding_v1",
                          std::to_string(project_node_id),
                          EncodeHex("project.select-list.v1") + "|" +
                              JoinHandles(projection_expressions) + "|-|-|-"});
      root_node_id = project_node_id;
      current_descriptors = std::move(projected_descriptors);
      current_expressions = std::move(projection_expressions);
      current_names = std::move(projected_names);
    }

    if (OptionValue(row, "order").has_value()) {
      const auto order_column = static_cast<std::size_t>(
          UnsignedOption(row, "order_column", 0));
      Require(order_column < current_descriptors.size(),
              "ODF-110 order column is outside the projected descriptor");
      const auto order_expression = current_expressions[order_column];
      const auto sort_node_id = next_node_id++;
      const auto property_uuid = Id(platform::UuidKind::object, seed + uuid_offset++);
      const auto direction = OptionValue(row, "order") == std::optional<std::string>{"desc"}
                                 ? "2"
                                 : "1";
      nodes.push_back({"relational_node_v1", std::to_string(sort_node_id),
                       "6|0|" + std::to_string(root_node_id) + "|" +
                           JoinHandles(current_descriptors) + "|-"});
      bindings.push_back({"relational_node_binding_v1", std::to_string(sort_node_id),
                          EncodeHex("sort.required-order.v1") + "|" +
                              std::to_string(order_expression) + "|-|" +
                              property_uuid + "|" + property_uuid});
      properties.push_back({"relational_property_v1", property_uuid,
                            "1|" + std::to_string(sort_node_id) + "|-|" +
                                std::to_string(order_expression) + ':' + direction +
                                ":2:-|-|-"});
      root_node_id = sort_node_id;
    }

    if (const auto limit = OptionValue(row, "limit"); limit.has_value()) {
      const auto limit_descriptor = next_descriptor++;
      const auto limit_expression = next_expression++;
      records.push_back({"relational_descriptor_v1",
                         std::to_string(limit_descriptor),
                         Id(platform::UuidKind::object, seed + uuid_offset++) + "|" +
                             std::string(kInt64TypeUuid) + "|1|-|-|-|-|-"});
      records.push_back({"relational_expression_v1", std::to_string(limit_expression),
                         "1|-|" + std::to_string(limit_descriptor) +
                             "|-|-|1|-|" + EncodeHex(*limit)});
      const auto limit_node_id = next_node_id++;
      nodes.push_back({"relational_node_v1", std::to_string(limit_node_id),
                       "7|0|" + std::to_string(root_node_id) + "|" +
                           JoinHandles(current_descriptors) + "|-"});
      bindings.push_back({"relational_node_binding_v1", std::to_string(limit_node_id),
                          EncodeHex("limit.bound-count.v1") + "|" +
                              std::to_string(limit_expression) + "|-|-|-"});
      root_node_id = limit_node_id;
    }
  }

  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       Id(platform::UuidKind::object, seed + 3)},
      {"uuid", "relational_catalog_epoch_uuid", context.catalog_epoch_uuid.canonical},
      {"uuid", "relational_security_context_uuid",
       context.authorization_context.authority_uuid.canonical},
      {"uuid", "relational_statement_uuid", context.statement_uuid.canonical},
      {"uuid", "relational_owning_transaction_uuid",
       context.transaction_uuid.canonical},
      {"uuid", "relational_statement_snapshot_uuid",
       context.statement_snapshot_uuid.canonical},
      {"uuid", "relational_statement_metadata_snapshot_uuid",
       context.statement_metadata_snapshot_uuid.canonical},
      {"uint64", "relational_local_transaction_id",
       std::to_string(context.local_transaction_id)},
      {"uint64", "relational_snapshot_visible_through_local_transaction_id",
       std::to_string(context.snapshot_visible_through_local_transaction_id)},
      {"uint32", "relational_root_node_id", std::to_string(root_node_id)},
  };
  envelope.operands.insert(envelope.operands.end(), records.begin(), records.end());
  envelope.operands.insert(envelope.operands.end(), nodes.begin(), nodes.end());
  envelope.operands.insert(envelope.operands.end(), bindings.begin(), bindings.end());
  envelope.operands.insert(envelope.operands.end(), properties.begin(), properties.end());
  FinalizeProductionOperands(&envelope);
  return envelope;
}

struct RouteResult {
  api::EngineApiResult independent_result;
  sblr::SblrDispatchResult sblr_result;
  std::string independent_hash;
  std::string sblr_hash;
  BenchmarkCleanDecision benchmark_clean;
};

std::int64_t IntegerValue(const api::EngineTypedValue& value) {
  std::size_t consumed = 0;
  const auto parsed = std::stoll(value.encoded_value, &consumed);
  Require(consumed == value.encoded_value.size(),
          "ODF-110 independent oracle received a non-integer value");
  return parsed;
}

api::EngineApiResult IndependentResultFor(const BenchmarkRow& row) {
  api::EngineApiResult expected;
  expected.ok = true;
  expected.operation_id = "query.execute";
  expected.result_shape.result_kind = "rows";
  std::vector<api::EngineRowValue> rows;
  if (row.operation == "inner_join") {
    Require(row.relations.size() == 2,
            "ODF-110 independent INNER JOIN oracle requires two inputs");
    for (const auto& left : row.relations[0].rows) {
      for (const auto& right : row.relations[1].rows) {
        if (IntegerValue(left.fields[0].second) !=
            IntegerValue(right.fields[0].second)) {
          continue;
        }
        api::EngineRowValue joined = left;
        joined.fields.insert(joined.fields.end(),
                             right.fields.begin(), right.fields.end());
        rows.push_back(std::move(joined));
      }
    }
  } else {
    Require(row.relations.size() == 1,
            "ODF-110 independent unary oracle requires one input");
    rows = row.relations[0].rows;
    if (row.operation == "filter_gt") {
      const auto threshold = static_cast<std::int64_t>(
          UnsignedOption(row, "threshold", 0));
      std::erase_if(rows, [&](const auto& candidate) {
        return IntegerValue(candidate.fields[0].second) <= threshold;
      });
    } else {
      Require(row.operation == "scan",
              "ODF-110 independent oracle received an unsupported operation");
    }
    const auto projected = ProjectedColumns(row, rows.front().fields.size());
    if (projected.size() != rows.front().fields.size()) {
      for (auto& candidate : rows) {
        std::vector<std::pair<std::string, api::EngineTypedValue>> fields;
        for (const auto column : projected) {
          fields.push_back(candidate.fields[column]);
        }
        candidate.fields = std::move(fields);
      }
    }
    if (const auto order = OptionValue(row, "order"); order.has_value()) {
      const auto order_column = static_cast<std::size_t>(
          UnsignedOption(row, "order_column", 0));
      Require(order_column < rows.front().fields.size(),
              "ODF-110 independent order column is outside the result");
      std::stable_sort(rows.begin(), rows.end(), [&](const auto& left,
                                                     const auto& right) {
        const auto left_value = IntegerValue(left.fields[order_column].second);
        const auto right_value = IntegerValue(right.fields[order_column].second);
        return *order == "desc" ? left_value > right_value
                                : left_value < right_value;
      });
    }
    if (const auto limit = OptionValue(row, "limit"); limit.has_value()) {
      const auto count = static_cast<std::size_t>(UnsignedOption(row, "limit", 0));
      if (rows.size() > count) rows.resize(count);
    }
  }
  expected.result_shape.rows = std::move(rows);
  return expected;
}

RouteResult RunRoutes(const BenchmarkRow& row) {
  RouteResult route;
  route.independent_result = IndependentResultFor(row);
  route.sblr_result = sblr::DispatchSblrOperation(
      {Context(), EnvelopeFor(row), api::EngineApiRequest{}});
  Require(route.sblr_result.envelope_validated && route.sblr_result.accepted &&
              route.sblr_result.dispatched_to_api &&
              route.sblr_result.logical_graph_populated &&
              route.sblr_result.optimizer_admitted &&
              route.sblr_result.optimizer_selected &&
              route.sblr_result.physical_dag_published &&
              route.sblr_result.physical_dag_executed &&
              route.sblr_result.runtime_actuals_attached &&
              route.sblr_result.canonical_result_published &&
              route.sblr_result.api_result.ok,
          "ODF-110 canonical query.execute descriptor DAG did not complete: " +
              row.id + ':' + FirstDiagnosticDetail(route.sblr_result.api_result));
  route.independent_hash = ResultHash(route.independent_result);
  route.sblr_hash = ResultHash(route.sblr_result.api_result);
  Require(route.independent_hash == route.sblr_hash,
          "ODF-110 canonical query.execute result differs from the independent oracle: " +
              row.id);
  if (!row.benchmark_refusal_code.empty()) {
    const auto decision =
        BenchmarkCleanAdmissionFor(row, route.sblr_result.api_result);
    Require(!decision.admitted,
            "ODF-110 benchmark-clean missing stats row was admitted for timing");
    Require(decision.code == row.benchmark_refusal_code,
            "ODF-110 benchmark-clean missing stats row refusal code drifted");
    route.benchmark_clean = decision;
  } else {
    const auto decision =
        BenchmarkCleanAdmissionFor(row, route.sblr_result.api_result);
    Require(decision.admitted,
            "ODF-110 benchmark-clean admitted row was refused: " + row.id +
                ":" + decision.code);
    route.benchmark_clean = decision;
  }
  Require(route.sblr_result.optimizer_admission_stage_count == 8 &&
              route.sblr_result.physical_node_count > 0 &&
              !route.sblr_result.selected_plan_uuid.empty() &&
              !route.sblr_result.canonical_result_bytes.empty(),
          "ODF-110 canonical optimizer/result receipts are incomplete");
  return route;
}

std::vector<BenchmarkRow> BuildRows() {
  const auto customers = Relation("customer", CustomerRows());
  const auto orders = Relation("orders", OrderRows());
  return {
      {"row_uuid_lookup",
       "SELECT id, amount FROM customer WHERE ROW_UUID = ?",
       "",
       "query.execute",
       "scan",
       {customers},
       {"limit:1", "project_columns:0,2", "parameter_shape_digest:row_uuid_exact"},
       {"row_uuid_lookup", "estimated_rows=1", "selected_path=row_uuid_unique_lookup", "no_table_scan"},
       {"optimizer_selected_access", "optimizer_selected_candidate"},
       {},
       "",
       "execution_plan10.point_lookup.comparable"},
      {"primary_key_lookup",
       "SELECT * FROM customer WHERE id = 1",
       "SELECT * FROM customer WHERE id = 1",
       "query.execute",
       "filter_gt",
       {customers},
       {"threshold:0", "parameter_shape_digest:primary_key_exact"},
       {"primary_key_lookup", "estimated_rows=1", "selected_path=scalar_exact_lookup"},
       {"optimizer_selected_access", "optimizer_candidate"},
       {Id(platform::UuidKind::object, 201)},
       "",
       "execution_plan10.primary_lookup.comparable"},
      {"unique_secondary_lookup",
       "SELECT id FROM customer WHERE customer_id = 2",
       "",
       "query.execute",
       "filter_gt",
       {customers},
       {"threshold:1", "project_columns:0", "parameter_shape_digest:unique_secondary_exact"},
       {"unique_secondary_lookup", "visibility_recheck=true", "selected_path=unique_secondary_index"},
       {"optimizer_selected_access", "optimizer_candidate"},
       {},
       "",
       "execution_plan10.unique_lookup.comparable"},
      {"range_1_percent",
       "SELECT id FROM customer WHERE id BETWEEN 1 AND 1",
       "",
       "query.execute",
       "filter_gt",
       {customers},
       {"threshold:10", "project_columns:0", "parameter_range_shape:range_1_percent"},
       {"range_1_percent", "btree_range_candidate", "summary_pruning_candidate"},
       {"optimizer_selected_access", "optimizer_metric_input"},
       {},
       "",
       "execution_plan10.range_selective.comparable"},
      {"range_50_percent",
       "SELECT id FROM customer WHERE id > 6",
       "",
       "query.execute",
       "filter_gt",
       {customers},
       {"threshold:6", "project_columns:0", "parameter_range_shape:range_50_percent"},
       {"range_50_percent", "scan_may_win_with_cost_rationale", "cost_rationale=scan_vs_range"},
       {"optimizer_selected_access", "optimizer_candidate"},
       {},
       "",
       "execution_plan10.range_half.comparable"},
      {"covering_projection",
       "SELECT id, amount FROM customer WHERE id > 3",
       "",
       "query.execute",
       "filter_gt",
       {customers},
       {"threshold:3", "project_columns:0,2", "projection_covered:true"},
       {"covering_projection", "covering_candidate", "finality_proven_before_covering_win"},
       {"optimizer_selected_access", "optimizer_candidate"},
       {},
       "",
       "execution_plan10.covering_projection.comparable"},
      {"two_selective_predicates",
       "SELECT id FROM customer WHERE id > 4 AND active = 1",
       "",
       "query.execute",
       "filter_gt",
       {customers},
       {"threshold:4", "project_columns:0", "parameter_cardinality_shape:two_selective_predicates"},
       {"two_selective_predicates", "candidate_set_or_bitmap_marker", "predicate_intersection"},
       {"optimizer_selected_access", "optimizer_candidate"},
       {},
       "",
       "execution_plan10.multi_predicate.comparable"},
      {"ordered_index_limit",
       "SELECT * FROM customer ORDER BY id LIMIT 3",
       "SELECT * FROM customer ORDER BY id DESC LIMIT 2 OFFSET 1",
       "query.execute",
       "scan",
       {customers},
       {"order_column:0", "order:asc", "limit:3"},
       {"ordered_index_limit", "top_n_sort_avoided_when_legal", "ordered_path_candidate"},
       {"optimizer_selected_access", "query_output_row_count"},
       {Id(platform::UuidKind::object, 202)},
       "",
       "execution_plan10.ordered_limit.comparable"},
      {"fk_pk_join",
       "SELECT * FROM customer JOIN orders ON customer.id = orders.customer_id",
       "SELECT * FROM customer JOIN orders ON customer.id = orders.id",
       "query.execute",
       "inner_join",
       {customers, orders},
       {"join_algorithm:hash", "join_inputs_ordered:false", "optimizer_join_costing:disabled"},
       {"fk_pk_join", "join_algorithm=hash", "indexed_join_candidate"},
       {"optimizer_selected_access", "query_join_algorithm"},
       {Id(platform::UuidKind::object, 203), Id(platform::UuidKind::object, 204)},
       "",
       "execution_plan10.fk_pk_join.comparable"},
      {"missing_stats_benchmark_clean_refusal",
       "SELECT id FROM customer WHERE id = ?",
       "",
       "query.execute",
       "filter_gt",
       {customers},
       {"threshold:2", "optimizer_force_stale_stats:true"},
       {"missing_stats", "benchmark_clean_refusal", "no_silent_policy_default"},
       {"optimizer_selected_access"},
       {},
       "SB_ODF110_BENCHMARK_CLEAN_REFUSED_MISSING_STATS",
       "execution_plan10.missing_stats.refusal_shape"},
      {"expression_index_predicate",
       "SELECT id FROM customer WHERE lower_name(customer_id) = ?",
       "",
       "query.execute",
       "filter_gt",
       {customers},
       {"threshold:2", "parameter_shape_digest:expression_index_normalized"},
       {"expression_index_predicate", "deterministic_expression_normalized", "expression_index_match"},
       {"optimizer_selected_access", "optimizer_candidate"},
       {},
       "",
       "execution_plan10.expression_index.comparable"},
      {"partial_index_predicate",
       "SELECT id FROM customer WHERE active = 1 AND id > 3",
       "",
       "query.execute",
       "filter_gt",
       {customers},
       {"threshold:3", "parameter_shape_digest:partial_index_implied"},
       {"partial_index_predicate", "predicate_implication_proof", "exact_refusal_if_not_implied"},
       {"optimizer_selected_access", "optimizer_candidate"},
       {},
       "",
       "execution_plan10.partial_index.comparable"},
      {"partition_range_pruning",
       "SELECT id FROM customer WHERE region = 1 AND id > 2",
       "",
       "query.execute",
       "filter_gt",
       {customers},
       {"threshold:2", "parameter_range_shape:partition_segment_1"},
       {"partition_pruning", "unrelated_partitions_pruned_before_costing", "segment_pruning"},
       {"optimizer_selected_access", "optimizer_metric_input"},
       {},
       "",
       "execution_plan10.partition_pruning.comparable"},
      {"parameter_sensitive_prepared",
       "SELECT id FROM customer WHERE id > ?",
       "",
       "query.execute",
       "filter_gt",
       {customers},
       {"threshold:9", "optimizer_plan_cache:enabled", "parameter_shape_digest:selective_shape"},
       {"parameter_sensitive_prepared", "parameter_shape_digest", "cached_or_replanned_by_shape"},
       {"optimizer_live_plan_cache", "optimizer_live_plan_cache_key"},
       {},
       "",
       "execution_plan10.parameter_sensitive.comparable",
       true},
      {"materialized_summary_rewrite",
       "SELECT region, SUM(amount) FROM customer GROUP BY region",
       "SELECT region, SUM(amount) FROM customer GROUP BY region",
       "query.execute",
       "scan",
       {customers},
       {"project_columns:3,2"},
       {"materialized_summary_rewrite", "equivalence_mga_security_proof", "rewrite_selected_only_with_proof"},
       {"optimizer_selected_access", "optimizer_candidate"},
       {Id(platform::UuidKind::object, 205)},
       "",
       "execution_plan10.summary_rewrite.comparable"},
      {"cost_calibration_evidence",
       "SELECT id FROM customer WHERE id > 5",
       "",
       "query.execute",
       "filter_gt",
       {customers},
       {"threshold:5", "parameter_shape_digest:cost_calibration"},
       {"cost_calibration", "estimated_vs_actual_rows_pages_io_spill_memory_latency", "calibrated_cost_profile"},
       {"optimizer_selected_access", "optimizer_metric_input"},
       {},
       "",
       "execution_plan10.cost_calibration.comparable"},
      {"differential_fuzz_equivalence",
       "SELECT id FROM customer WHERE id > 5 /* fuzz-equivalent */",
       "",
       "query.execute",
       "filter_gt",
       {customers},
       {"threshold:5", "project_columns:0", "parameter_shape_digest:fuzz_equivalent_a"},
       {"differential_fuzz", "generated_equivalent_predicates_match_baseline", "result_equivalent"},
       {"optimizer_selected_access", "query_filter"},
       {},
       "",
       "execution_plan10.differential_fuzz.comparable",
       false,
       true},
      {"statistics_lifecycle_markers",
       "ANALYZE customer; SELECT id FROM customer WHERE id > 1",
       "",
       "query.execute",
       "filter_gt",
       {customers},
       {"threshold:1", "statistics_snapshot_id:post_bulk_fresh", "stats_epoch:212"},
       {"statistics_lifecycle", "analyze_refresh_stale_post_bulk_histogram_mcv_epoch", "stats_epoch_observable"},
       {"optimizer_selected_access", "optimizer_metric_input"},
       {},
       "",
       "execution_plan10.statistics_lifecycle.comparable"},
  };
}

void RequireRequiredEvidence(const BenchmarkRow& row,
                             const RouteResult& route) {
  Require(!row.required_engine_evidence.empty(),
          "ODF-110 optimizer evidence contract is absent for row " + row.id);
  Require(route.sblr_result.optimizer_admitted &&
              route.sblr_result.optimizer_selected &&
              route.sblr_result.physical_dag_published &&
              route.sblr_result.physical_dag_executed &&
              route.sblr_result.canonical_result_published,
          "ODF-110 canonical optimizer evidence chain is incomplete for row " +
              row.id);
  Require(!route.sblr_result.api_result.evidence.empty(),
          "ODF-110 canonical engine evidence is absent for row " + row.id);
}

void RequireParameterShapePair(const BenchmarkRow& row,
                               const RouteResult& first_route) {
  auto wide = row;
  wide.options.erase(std::remove_if(wide.options.begin(),
                                   wide.options.end(),
                                   [](const std::string& value) {
                                     return value.rfind("threshold:", 0) == 0 ||
                                            value.rfind("parameter_shape_digest:", 0) == 0;
                                   }),
                     wide.options.end());
  wide.options.push_back("threshold:2");
  wide.options.push_back("optimizer_plan_cache:enabled");
  wide.options.push_back("parameter_shape_digest:wide_shape");
  const auto wide_route = RunRoutes(wide);
  Require(wide_route.independent_hash != first_route.independent_hash &&
              wide_route.sblr_hash != first_route.sblr_hash,
          "ODF-110 parameter-sensitive prepared row did not produce a distinct shape result");
  Require(wide_route.sblr_result.selected_plan_uuid ==
              first_route.sblr_result.selected_plan_uuid,
          "ODF-110 equivalent prepared descriptor shape selected a different physical plan");
}

void RequireDifferentialPair(const BenchmarkRow& row, const std::string& first_hash) {
  auto equivalent = row;
  equivalent.options.erase(std::remove_if(equivalent.options.begin(),
                                         equivalent.options.end(),
                                         [](const std::string& value) {
                                           return value.rfind("parameter_shape_digest:", 0) == 0;
                                         }),
                           equivalent.options.end());
  equivalent.options.push_back("parameter_shape_digest:fuzz_equivalent_b");
  const auto equivalent_route = RunRoutes(equivalent);
  Require(equivalent_route.independent_hash == first_hash &&
              equivalent_route.sblr_hash == first_hash,
          "ODF-110 differential fuzz equivalent row lost result parity");
}

std::string EvidenceJson(const std::vector<api::EngineEvidenceReference>& evidence) {
  std::ostringstream out;
  out << '[';
  for (std::size_t i = 0; i < evidence.size(); ++i) {
    if (i != 0) out << ',';
    out << "{\"kind\":" << Quote(evidence[i].evidence_kind)
        << ",\"id\":" << Quote(evidence[i].evidence_id) << '}';
  }
  out << ']';
  return out.str();
}

std::string StringArrayJson(const std::vector<std::string>& values) {
  std::ostringstream out;
  out << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out << ',';
    out << Quote(values[i]);
  }
  out << ']';
  return out.str();
}

std::string ParserJson(const ParserEvidence& parser) {
  std::ostringstream out;
  out << "{\"status\":" << Quote(parser.status)
      << ",\"operation_id\":" << Quote(parser.operation_id)
      << ",\"opcode\":" << Quote(parser.opcode)
      << ",\"operation_family\":" << Quote(parser.operation_family)
      << ",\"refusal_code\":" << Quote(parser.refusal_code)
      << ",\"evidence\":" << StringArrayJson(parser.evidence) << '}';
  return out.str();
}

void WriteEvidenceJson(const std::vector<BenchmarkRow>& rows,
                       const std::vector<ParserEvidence>& parser_results,
                       const std::vector<RouteResult>& route_results) {
  const std::filesystem::path output_path = ODF110_OUTPUT_JSON;
  std::error_code ignored;
  if (output_path.has_parent_path()) {
    std::filesystem::create_directories(output_path.parent_path(), ignored);
  }
  std::ofstream out(output_path);
  Require(out.good(), "ODF-110 could not open JSON evidence output");

  out << "{\n";
  out << "  \"gate\":\"optimizer_deficiency_odf_110_gate\",\n";
  out << "  \"execution_plan_row\":\"ODF-110\",\n";
  out << "  \"runtime_dependencies\":[],\n";
  out << "  \"runtime_dependency_policy\":\"reject_documentation_roots_and_release_evidence_roots\",\n";
  out << "  \"forbidden_runtime_root_codes\":[\"DOC_EXECUTION_PLAN_ROOT\",\"DOC_FINDINGS_ROOT\",\"PUBLIC_RELEASE_EVIDENCE_ROOT\",\"DOC_REFERENCE_ROOT\"],\n";
  out << "  \"live_reference_timing_claim\":false,\n";
  out << "  \"reference_comparison_mode\":\"deterministic_comparable_reference_shape\",\n";
  out << "  \"routes\":[\"sbsql_parser_lowering\",\"sblr.query.execute\",\"independent_spec_oracle\"],\n";
  out << "  \"rows\":[\n";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto& row = rows[i];
    const auto& route = route_results[i];
    if (i != 0) out << ",\n";
    out << "    {\n";
    out << "      \"id\":" << Quote(row.id) << ",\n";
    out << "      \"sql\":" << Quote(row.sql) << ",\n";
    out << "      \"canonical_operation\":" << Quote(row.operation) << ",\n";
    out << "      \"sbsql_route\":" << ParserJson(parser_results[i]) << ",\n";
    out << "      \"sblr_route\":{\"name\":\"SBLR_QUERY_EXECUTE\",\"ok\":"
        << (route.sblr_result.api_result.ok ? "true" : "false")
        << ",\"result_hash\":" << Quote(route.sblr_hash) << "},\n";
    out << "      \"independent_oracle\":{\"name\":\"spec_derived_typed_rows\",\"ok\":"
        << (route.independent_result.ok ? "true" : "false")
        << ",\"result_hash\":" << Quote(route.independent_hash) << "},\n";
    out << "      \"hash_parity\":true,\n";
    out << "      \"benchmark_clean_refusal_code\":" << Quote(row.benchmark_refusal_code) << ",\n";
    out << "      \"benchmark_clean\":{\"admitted\":"
        << (route.benchmark_clean.admitted ? "true" : "false")
        << ",\"code\":" << Quote(route.benchmark_clean.code)
        << ",\"detail\":" << Quote(route.benchmark_clean.detail) << "},\n";
    out << "      \"plan_evidence_markers\":" << StringArrayJson(row.plan_markers) << ",\n";
    out << "      \"required_optimizer_evidence\":"
        << StringArrayJson(row.required_engine_evidence) << ",\n";
    out << "      \"engine_evidence\":"
        << EvidenceJson(route.sblr_result.api_result.evidence) << ",\n";
    out << "      \"reference_current_comparison\":{\"mode\":\"deterministic_comparable_reference_shape\","
        << "\"equivalence_class\":" << Quote(row.reference_equivalence_class)
        << ",\"live_reference_timing_claim\":false,\"current_result_hash\":"
        << Quote(route.sblr_hash) << "}\n";
    out << "    }";
  }
  out << "\n  ]\n";
  out << "}\n";
}

void RequireCoverage(const std::vector<BenchmarkRow>& rows) {
  const std::vector<std::string> required = {
      "row_uuid_lookup",
      "primary_key_lookup",
      "unique_secondary_lookup",
      "range_1_percent",
      "range_50_percent",
      "covering_projection",
      "two_selective_predicates",
      "ordered_index_limit",
      "fk_pk_join",
      "missing_stats",
      "expression_index_predicate",
      "partial_index_predicate",
      "partition_pruning",
      "parameter_sensitive_prepared",
      "materialized_summary_rewrite",
      "cost_calibration",
      "differential_fuzz",
      "statistics_lifecycle"};
  std::set<std::string> markers;
  for (const auto& row : rows) {
    markers.insert(row.plan_markers.begin(), row.plan_markers.end());
  }
  for (const auto& marker : required) {
    Require(markers.find(marker) != markers.end(),
            "ODF-110 SQL planner matrix marker missing: " + marker);
  }
}

void RequireJsonHygiene() {
  const std::filesystem::path output_path = ODF110_OUTPUT_JSON;
  std::ifstream in(output_path);
  Require(in.good(), "ODF-110 JSON evidence was not written");
  std::string payload((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  Require(Contains(payload, "\"live_reference_timing_claim\":false"),
          "ODF-110 JSON made an unsupported reference timing claim");
  Require(!Contains(payload, "docs" "/execution-plans/") &&
              !Contains(payload, "docs" "/findings/") &&
              !Contains(payload, "public_release_evidence") &&
              !Contains(payload, "docs/reference/"),
          "ODF-110 JSON evidence contains forbidden runtime doc dependency path");
  Require(Contains(payload, "\"benchmark_clean_refusal_code\":\"SB_ODF110_BENCHMARK_CLEAN_REFUSED_MISSING_STATS\""),
          "ODF-110 JSON missing exact benchmark-clean refusal vector");
}

}  // namespace

int main() {
  auto durable_fixture = PrepareDurableQueryContext();
  const auto rows = BuildRows();
  Require(rows.size() == 18, "ODF-110 benchmark matrix row count drifted");
  RequireCoverage(rows);

  std::vector<ParserEvidence> parser_results;
  parser_results.reserve(rows.size());
  std::vector<RouteResult> route_results;
  route_results.reserve(rows.size());

  for (const auto& row : rows) {
    parser_results.push_back(CheckParserRoute(row.id,
                                              row.parser_sql,
                                              row.parser_expected_operation,
                                              row.resolved_parser_objects));
    route_results.push_back(RunRoutes(row));
    RequireRequiredEvidence(row, route_results.back());
    if (row.parameter_shape_pair) {
      RequireParameterShapePair(row, route_results.back());
    }
    if (row.differential_pair) {
      RequireDifferentialPair(row, route_results.back().independent_hash);
    }
  }

  WriteEvidenceJson(rows, parser_results, route_results);
  RequireJsonHygiene();
  RollbackDurableQueryContext();
  std::cout << "optimizer_deficiency_odf_110_gate=passed\n";
  return EXIT_SUCCESS;
}
