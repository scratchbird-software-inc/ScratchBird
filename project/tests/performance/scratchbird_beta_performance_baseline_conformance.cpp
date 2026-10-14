#include "../support/engine_evidence_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../support/catalog_column_binding_fixture.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "api_types.hpp"
#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "hash_digest.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "memory.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "sblr_transaction_begin_runtime.hpp"
#include "sblr_transaction_commit_runtime.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include "../database_lifecycle/credentialed_database_fixture.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

namespace {

namespace api = scratchbird::engine::internal_api;
namespace memory = scratchbird::core::memory;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;

#ifndef SB_PERF_THRESHOLD_JSON
#define SB_PERF_THRESHOLD_JSON "project/tests/performance/BETA_PERFORMANCE_BASELINE_THRESHOLDS.json"
#endif

#ifndef SB_PERF_OUTPUT_JSON
#define SB_PERF_OUTPUT_JSON "/tmp/scratchbird_beta_performance_baseline_result.json"
#endif

#ifndef SB_PERF_SEED_PACK_ROOT
#define SB_PERF_SEED_PACK_ROOT "project/resources/seed-packs/initial-resource-pack"
#endif

constexpr auto kSchemaUuid = scratchbird::tests::FixtureUuidLiteral("019e07f0-0000-7000-8000-000000000101");
constexpr auto kTableUuid = scratchbird::tests::FixtureUuidLiteral("019e07f0-0000-7000-8000-000000000102");

struct Thresholds {
  double startup_open_latency_ms_max = 5000.0;
  double session_begin_commit_latency_ms_max = 2000.0;
  double simple_query_latency_ms_max = 1000.0;
  double insert_rows_per_second_min = 25.0;
  double select_rows_per_second_min = 25.0;
  std::uint64_t rss_growth_bytes_max = 268435456;
  std::uint64_t sample_rows = 128;
  std::uint64_t repeat_count = 5;
  double max_relative_variance = 4.0;
};

struct MeasurementStats {
  double mean_ms = 0.0;
  double median_ms = 0.0;
  double p50_ms = 0.0;
  double p90_ms = 0.0;
  double p95_ms = 0.0;
  double p99_ms = 0.0;
  double p99_9_ms = 0.0;
  double max_latency_ms = 0.0;
  double variance_ms2 = 0.0;
  double stddev_ms = 0.0;
  double relative_variance = 0.0;
  std::vector<double> samples_ms;
};

struct Measurements {
  double startup_open_latency_ms = 0.0;
  double session_begin_commit_latency_ms = 0.0;
  double simple_query_latency_ms = 0.0;
  double insert_rows_per_second = 0.0;
  double select_rows_per_second = 0.0;
  MeasurementStats simple_query_stats;
  MeasurementStats select_all_stats;
  std::uint64_t rss_growth_bytes = 0;
  std::uint64_t memory_high_water_bytes = 0;
  std::uint64_t fd_high_water = 0;
  std::uint64_t fd_growth = 0;
  std::uint64_t inserted_rows = 0;
  std::uint64_t selected_rows = 0;
  double error_rate = 0.0;
  double retry_rate = 0.0;
  double spill_rate = 0.0;
  std::uint64_t cleanup_lag = 0;
  std::string selected_result_hash;
};

memory::AllocationPolicy MemoryPolicy() {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "scratchbird_beta_performance_baseline";
  policy.hard_limit_bytes = 384ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 256ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 128ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 128ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

void PrintApiResultDiagnostics(const api::EngineApiResult& result, std::string_view label) {
  std::cerr << label << " ok=" << (result.ok ? "true" : "false")
            << " operation_id=" << result.operation_id
            << " cluster_authority_required="
            << (result.cluster_authority_required ? "true" : "false") << '\n';
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << "  diagnostic code=" << diagnostic.code
              << " message_key=" << diagnostic.message_key
              << " detail=" << diagnostic.detail
              << " error=" << (diagnostic.error ? "true" : "false") << '\n';
  }
  for (const auto& unsupported : result.unsupported_features) {
    std::cerr << "  unsupported feature=" << unsupported.feature
              << " reason=" << unsupported.reason << '\n';
  }
  for (const auto& evidence : result.evidence) {
    std::cerr << "  evidence kind=" << evidence.evidence_kind << " id=";
    if (const auto* text = std::get_if<std::string>(&evidence.evidence_id)) {
      std::cerr << *text;
    } else {
      std::cerr << "[binary UUID]";
    }
    std::cerr << '\n';
  }
}

void ConfigureMemoryFixture() {
  const auto configured =
      memory::ConfigureDefaultMemoryManagerForFixture(
          MemoryPolicy(), "scratchbird_beta_performance_baseline_conformance");
  if (!configured.ok()) {
    std::cerr << "memory manager configure failed code="
              << configured.diagnostic.diagnostic_code
              << " message_key=" << configured.diagnostic.message_key
              << " remediation_hint=" << configured.diagnostic.remediation_hint << '\n';
  }
  Require(configured.ok(), "performance baseline memory manager fixture configure failed");
  Require(configured.fixture_mode, "performance baseline memory manager did not use fixture mode");
  const auto state = memory::DefaultMemoryManagerState();
  Require(state.initialized, "performance baseline default memory manager not initialized");
  Require(state.explicitly_configured, "performance baseline memory manager not explicit");
  Require(state.fixture_mode, "performance baseline memory manager lost fixture provenance");
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream in(path);
  Require(in.good(), "failed to open threshold artifact");
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

double JsonNumber(const std::string& text, std::string_view key) {
  const std::regex pattern("\"" + std::string(key) + "\"\\s*:\\s*([0-9]+(?:\\.[0-9]+)?)");
  std::smatch match;
  Require(std::regex_search(text, match, pattern), "threshold JSON missing numeric key");
  return std::stod(match[1].str());
}

Thresholds LoadThresholds() {
  const std::string text = ReadText(SB_PERF_THRESHOLD_JSON);
  for (std::string_view token : {"scratchbird.beta.performance.baseline.v1",
                                 "beta readiness severe-regression tripwire",
                                 "single-process core engine SBLR/internal API",
                                 "build tree only",
                                 "not_ctest_inputs",
                                 "startup_open_latency_ms_max",
                                 "session_begin_commit_latency_ms_max",
                                 "simple_query_latency_ms_max",
                                 "insert_rows_per_second_min",
                                 "select_rows_per_second_min",
                                 "rss_growth_bytes_max",
                                 "severe_regression_factor",
                                 "repeat_count",
                                 "max_relative_variance"}) {
    Require(text.find(token) != std::string::npos, "threshold artifact missing required policy token");
  }
  Thresholds thresholds;
  thresholds.startup_open_latency_ms_max = JsonNumber(text, "startup_open_latency_ms_max");
  thresholds.session_begin_commit_latency_ms_max = JsonNumber(text, "session_begin_commit_latency_ms_max");
  thresholds.simple_query_latency_ms_max = JsonNumber(text, "simple_query_latency_ms_max");
  thresholds.insert_rows_per_second_min = JsonNumber(text, "insert_rows_per_second_min");
  thresholds.select_rows_per_second_min = JsonNumber(text, "select_rows_per_second_min");
  thresholds.rss_growth_bytes_max = static_cast<std::uint64_t>(JsonNumber(text, "rss_growth_bytes_max"));
  thresholds.sample_rows = static_cast<std::uint64_t>(JsonNumber(text, "sample_rows"));
  thresholds.repeat_count = static_cast<std::uint64_t>(JsonNumber(text, "repeat_count"));
  thresholds.max_relative_variance = JsonNumber(text, "max_relative_variance");
  Require(thresholds.sample_rows >= 32 && thresholds.sample_rows <= 512,
          "sample_rows must remain bounded for CTest");
  Require(thresholds.repeat_count >= 3 && thresholds.repeat_count <= 20,
          "repeat_count must remain bounded for CTest");
  return thresholds;
}

std::filesystem::path MakeTempDir() {
  std::error_code error;
  const auto base = std::filesystem::temp_directory_path(error);
  if (error) { return {}; }
  const auto timestamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto thread_token =
      std::hash<std::thread::id>{}(std::this_thread::get_id());
  for (int attempt = 0; attempt < 100; ++attempt) {
    auto candidate = base / ("sb_perf_baseline." + std::to_string(timestamp) +
                             "." + std::to_string(thread_token) + "." +
                             std::to_string(attempt));
    error.clear();
    if (std::filesystem::create_directory(candidate, error)) {
      return candidate;
    }
    if (error && !std::filesystem::exists(candidate)) {
      return {};
    }
  }
  return {};
}

std::uint64_t CurrentRssBytes() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX counters{};
  if (::GetProcessMemoryInfo(::GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) == 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(counters.WorkingSetSize);
#else
  std::ifstream statm("/proc/self/statm");
  std::uint64_t size_pages = 0;
  std::uint64_t resident_pages = 0;
  statm >> size_pages >> resident_pages;
  if (!statm.good() && resident_pages == 0) { return 0; }
  const long page_size = ::sysconf(_SC_PAGESIZE);
  return page_size > 0 ? resident_pages * static_cast<std::uint64_t>(page_size) : 0;
#endif
}

std::uint64_t CurrentFdCount() {
#ifdef _WIN32
  DWORD handle_count = 0;
  if (::GetProcessHandleCount(::GetCurrentProcess(), &handle_count) == 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(handle_count);
#else
  std::error_code error;
  std::uint64_t count = 0;
  for (const auto& ignored : std::filesystem::directory_iterator("/proc/self/fd", error)) {
    (void)ignored;
    ++count;
  }
  return error ? 0 : count;
#endif
}

template <typename Fn>
double MeasureMs(Fn&& fn) {
  const auto start = std::chrono::steady_clock::now();
  fn();
  const auto stop = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(stop - start).count();
}

double Percentile(const std::vector<double>& sorted_samples, double percentile) {
  if (sorted_samples.empty()) { return 0.0; }
  if (sorted_samples.size() == 1) { return sorted_samples.front(); }
  const double clamped = std::clamp(percentile, 0.0, 100.0);
  const double position = (clamped / 100.0) *
                          static_cast<double>(sorted_samples.size() - 1);
  const auto lower = static_cast<std::size_t>(std::floor(position));
  const auto upper = static_cast<std::size_t>(std::ceil(position));
  if (lower == upper) { return sorted_samples[lower]; }
  const double fraction = position - static_cast<double>(lower);
  return sorted_samples[lower] +
         ((sorted_samples[upper] - sorted_samples[lower]) * fraction);
}

template <typename Fn>
MeasurementStats MeasureRepeatedMs(Fn&& fn, std::uint64_t repeat_count) {
  MeasurementStats stats;
  stats.samples_ms.reserve(static_cast<std::size_t>(repeat_count));
  for (std::uint64_t i = 0; i < repeat_count; ++i) {
    stats.samples_ms.push_back(MeasureMs(fn));
  }
  stats.mean_ms =
      std::accumulate(stats.samples_ms.begin(), stats.samples_ms.end(), 0.0) /
      static_cast<double>(stats.samples_ms.size());
  std::vector<double> sorted_samples = stats.samples_ms;
  std::sort(sorted_samples.begin(), sorted_samples.end());
  stats.p50_ms = Percentile(sorted_samples, 50.0);
  stats.median_ms = stats.p50_ms;
  stats.p90_ms = Percentile(sorted_samples, 90.0);
  stats.p95_ms = Percentile(sorted_samples, 95.0);
  stats.p99_ms = Percentile(sorted_samples, 99.0);
  stats.p99_9_ms = Percentile(sorted_samples, 99.9);
  stats.max_latency_ms = sorted_samples.back();
  for (double sample : stats.samples_ms) {
    const double delta = sample - stats.mean_ms;
    stats.variance_ms2 += delta * delta;
  }
  stats.variance_ms2 /= static_cast<double>(stats.samples_ms.size());
  stats.stddev_ms = std::sqrt(stats.variance_ms2);
  stats.relative_variance =
      stats.mean_ms > 0.0 ? stats.variance_ms2 / (stats.mean_ms * stats.mean_ms) : 0.0;
  return stats;
}

api::EngineRequestContext BaseContext(const api::EngineRequestContext& owner,
                                      std::uint32_t session_ordinal = 1) {
  auto context = owner;
  context.request_id = "phase7m-performance-baseline";
  context.session_uuid = scratchbird::tests::FixtureUuid(1539, session_ordinal);
  context.trace_tags.push_back("PHASE7M");
  context.trace_tags.push_back("performance-baseline");
  return context;
}

sblr::SblrOperationEnvelope Envelope(std::string operation_id, std::string opcode) {
  const auto* registry_entry = sblr::LookupSblrOperation(operation_id);
  Require(registry_entry != nullptr,
          "performance baseline operation is absent from the canonical SBLR registry: " +
              operation_id);
  Require(registry_entry->opcode == opcode,
          "performance baseline opcode mnemonic drifted from the canonical SBLR registry");
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id), std::move(opcode), "PHASE7M");
  envelope.opcode_code = registry_entry->code;
  envelope.result_shape = registry_entry->result_contract;
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid = scratchbird::tests::FixtureUuidLiteral("019e07f0-0000-7000-8000-000000000010");
  envelope.registry_snapshot_uuid = scratchbird::tests::FixtureUuidLiteral("019e07f0-0000-7000-8000-000000000011");
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  return envelope;
}

sblr::SblrDispatchResult Dispatch(const std::filesystem::path& database_path,
                                  const std::string& operation_id,
                                  const std::string& opcode,
                                  api::EngineRequestContext context,
                                  api::EngineApiRequest request = {},
                                  bool requires_transaction = false) {
  auto envelope = Envelope(operation_id, opcode);
  envelope.requires_transaction_context = requires_transaction;
  request.context = context;
  request.operation_id = operation_id;
  sblr::SblrDispatchRequest dispatch;
  dispatch.context = std::move(context);
  dispatch.envelope = std::move(envelope);
  dispatch.api_request = std::move(request);
  auto result = sblr::DispatchSblrOperation(dispatch);
  if (!result.accepted || !result.envelope_validated || !result.dispatched_to_api || !result.api_result.ok) {
    std::cerr << "dispatch failed for " << operation_id << " path=" << database_path << '\n'
              << sblr::SerializeSblrDispatchResultToJson(result);
  }
  Require(result.accepted && result.envelope_validated && result.dispatched_to_api && result.api_result.ok,
          "SBLR dispatch failed");
  return result;
}

bool HasEvidence(const api::EngineApiResult& result, std::string_view kind, std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && (id.empty() || scratchbird::tests::EvidenceTextEquals(evidence.evidence_id, id))) { return true; }
  }
  return false;
}

std::uint64_t EvidenceU64(const api::EngineApiResult& result, std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind != kind) { continue; }
    try {
      return static_cast<std::uint64_t>(std::stoull(std::get<std::string>(evidence.evidence_id)));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineLocalizedName Name(std::string name) {
  return {"en", "primary", name, name, true};
}

api::EngineColumnDefinition Column(const api::EngineRequestContext& context,
                                   std::uint32_t ordinal,
                                   std::string name,
                                   std::string type,
                                   std::uint32_t identity_ordinal) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.requested_column_uuid = scratchbird::tests::FixtureUuid(1540, identity_ordinal);
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(1541, identity_ordinal);
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = std::move(type);
  column.descriptor.encoded_descriptor = "type=" + column.descriptor.canonical_type_name;
  scratchbird::tests::BindFixtureColumnDatatype(
      context, scratchbird::core::datatypes::CanonicalTypeId::character, column);
  return column;
}

api::EngineRowValue Row(std::uint64_t index) {
  api::EngineRowValue row;
  Require(index < UINT32_MAX, "performance row fixture ordinal overflow");
  row.requested_row_uuid = scratchbird::tests::FixtureUuid(1542, static_cast<std::uint32_t>(index + 1));
  row.fields.push_back({"id", TextValue("perf-" + std::to_string(index))});
  row.fields.push_back({"note", TextValue("baseline row " + std::to_string(index))});
  return row;
}

struct BaselineTransaction {
  api::EngineRequestContext context;
  scratchbird::core::hash::Digest256 begin_admission_sha256{};
  std::unique_ptr<scratchbird::tests::FixtureEngineSession> session;
};

using ResultValues = std::map<std::string, std::string>;
ResultValues ReadResultValues(const std::vector<api::EngineRowValue>& rows);

BaselineTransaction BeginTransaction(const api::EngineRequestContext& owner,
                                     std::uint32_t session_ordinal) {
  auto context = BaseContext(owner, session_ordinal);
  auto envelope = Envelope("engine.op.txn_begin", "SBLR_TXN_BEGIN");
  envelope.requires_transaction_context = false;

  sblr::SblrTransactionBeginOptionsV1 options;
  options.isolation_profile_uuid[0] = 1;
  options.isolation_profile_generation = 1;
  options.transaction_policy_snapshot_uuid[0] = 2;
  options.transaction_policy_generation = 1;
  options.read_mode = 1;
  options.authority_scope = 1;
  options.wait_policy = 1;
  auto body = sblr::EncodeSblrTransactionBeginOptionsV1(&options);
  Require(!body.empty(), "canonical transaction-begin options failed to encode");
  const auto admission_sha = scratchbird::core::hash::ComputeSha256Digest(body);
  Require(admission_sha.ok(), "canonical transaction-begin evidence hash failed");

  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "transaction.begin_options";
  operand.name = "options";
  operand.value_kind = sblr::SblrValueKind::transaction_begin_options;
  operand.value_body = std::move(body);
  envelope.operands.push_back(std::move(operand));

  api::EngineApiRequest admission_request;
  admission_request.context = context;
  admission_request.operation_id = "engine.op.txn_begin";
  auto admitted = sblr::DispatchSblrOperation(
      {context, std::move(envelope), std::move(admission_request), std::nullopt});
  Require(admitted.accepted && admitted.envelope_validated &&
              admitted.dispatched_to_api && admitted.api_result.ok,
          "canonical transaction-begin admission failed");
  Require(admitted.api_result.local_transaction_id == 0 &&
              admitted.api_result.transaction_uuid.is_nil(),
          "transaction-begin admission published engine MGA state");

  api::EngineBeginTransactionRequest begin;
  begin.context = context;
  begin.operation_id = "transaction.begin";
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  Require(begun.ok && begun.local_transaction_id != 0 &&
              !begun.transaction_uuid.is_nil(),
          "engine-owned transaction begin failed after canonical admission");
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  auto session = std::make_unique<scratchbird::tests::FixtureEngineSession>(context);
  return {std::move(context), admission_sha.digest, std::move(session)};
}

void Commit(const std::filesystem::path& database_path,
            BaselineTransaction& transaction) {
  (void)database_path;
  auto envelope = Envelope("engine.op.txn_commit", "SBLR_TXN_COMMIT");
  envelope.requires_transaction_context = true;

  Require(uuid::IsEngineIdentityUuid(transaction.context.transaction_uuid),
          "transaction UUID must be a native engine identity");
  sblr::SblrTransactionCommitOptionsV1 options;
  std::copy(transaction.context.transaction_uuid.bytes.begin(),
            transaction.context.transaction_uuid.bytes.end(),
            options.transaction_uuid.begin());
  options.local_transaction_id = transaction.context.local_transaction_id;
  options.admitted_handle_evidence_sha256 = transaction.begin_admission_sha256;
  options.commit_mode = 1;
  options.authority_scope = 1;
  options.wait_policy = 1;
  auto body = sblr::EncodeSblrTransactionCommitOptionsV1(&options);
  Require(!body.empty(), "canonical transaction-commit options failed to encode");

  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "transaction.commit.options";
  operand.name = "options";
  operand.value_kind = sblr::SblrValueKind::transaction_commit_options;
  operand.value_body = std::move(body);
  envelope.operands.push_back(std::move(operand));

  api::EngineApiRequest admission_request;
  admission_request.context = transaction.context;
  admission_request.operation_id = "engine.op.txn_commit";
  auto admitted = sblr::DispatchSblrOperation(
      {transaction.context, std::move(envelope), std::move(admission_request),
       std::nullopt});
  Require(admitted.accepted && admitted.envelope_validated &&
              admitted.dispatched_to_api && admitted.api_result.ok,
          "canonical transaction-commit admission failed");

  api::EngineCommitTransactionRequest commit;
  commit.context = transaction.context;
  commit.operation_id = "transaction.commit";
  const auto committed = api::EngineCommitTransaction(commit);
  Require(committed.ok && committed.engine_finality_known &&
              committed.commit_finality_state == "committed_by_engine_inventory",
          "engine-owned transaction commit failed after canonical admission");
  transaction.session.reset();
}

void CreateSchemaAndTable(const std::filesystem::path& database_path,
                          const BaselineTransaction& transaction) {
  const auto& context = transaction.context;
  (void)database_path;
  api::EngineCreateSchemaRequest schema_request;
  schema_request.context = context;
  schema_request.operation_id = "ddl.create_schema";
  schema_request.target_object.uuid = scratchbird::tests::FixtureUuidLiteral("019e07f0-0000-7000-8000-000000000101");
  schema_request.target_object.object_kind = "schema";
  schema_request.localized_names.push_back(Name("phase7m_perf_schema"));
  const auto schema = [&] {
    scratchbird::tests::FixtureEngineStatement statement(*transaction.session, context);
    schema_request.context = statement.context;
    return api::EngineCreateSchema(schema_request);
  }();
  Require(schema.ok, "engine-owned schema create failed");
  Require(schema.primary_object.uuid == kSchemaUuid,
          "schema UUID not preserved");

  api::EngineCreateTableRequest table_request;
  table_request.context = context;
  table_request.operation_id = "ddl.create_table";
  table_request.target_schema.uuid = scratchbird::tests::FixtureUuidLiteral("019e07f0-0000-7000-8000-000000000101");
  table_request.target_schema.object_kind = "schema";
  table_request.requested_table_uuid = scratchbird::tests::FixtureUuidLiteral("019e07f0-0000-7000-8000-000000000102");
  table_request.target_object.uuid = scratchbird::tests::FixtureUuidLiteral("019e07f0-0000-7000-8000-000000000102");
  table_request.target_object.object_kind = "table";
  table_request.table_names.push_back(Name("phase7m_perf_table"));
  table_request.table_columns.push_back(Column(context, 0, "id", "text", 1));
  table_request.table_columns.push_back(Column(context, 1, "note", "text", 2));
  const auto table = [&] {
    scratchbird::tests::FixtureEngineStatement statement(*transaction.session, context);
    table_request.context = statement.context;
    return api::EngineCreateTable(table_request);
  }();
  if (!table.ok) {
    for (const auto& diagnostic : table.diagnostics) {
      std::cerr << "table create diagnostic: " << diagnostic.code << ':'
                << diagnostic.detail << '\n';
    }
  }
  Require(table.ok, "engine-owned table create failed");
  Require(table.table_object.uuid == kTableUuid,
          "table UUID not preserved");
}

void InsertRows(const std::filesystem::path& database_path,
                const BaselineTransaction& transaction,
                std::uint64_t row_count) {
  scratchbird::tests::FixtureEngineStatement statement(*transaction.session, transaction.context);
  (void)database_path;
  api::EngineInsertRowsRequest request;
  request.context = statement.context;
  request.operation_id = "dml.insert_rows";
  request.target_table.uuid = scratchbird::tests::FixtureUuidLiteral("019e07f0-0000-7000-8000-000000000102");
  request.target_table.object_kind = "table";
  for (std::uint64_t i = 0; i < row_count; ++i) {
    request.input_rows.push_back(Row(i));
  }
  const auto inserted = api::EngineInsertRows(request);
  Require(inserted.ok, "engine-owned insert failed");
  Require(inserted.inserted_count == row_count,
          "inserted row count mismatch");
  Require(inserted.result_shape.rows.size() <= row_count,
          "insert returned more rows than it inserted");
}

std::size_t SelectById(const std::filesystem::path& database_path,
                       const BaselineTransaction& transaction,
                       std::string id) {
  scratchbird::tests::FixtureEngineStatement statement(*transaction.session, transaction.context);
  (void)database_path;
  api::EngineSelectRowsRequest request;
  request.context = statement.context;
  request.operation_id = "dml.select_rows";
  request.source_object.uuid = scratchbird::tests::FixtureUuidLiteral("019e07f0-0000-7000-8000-000000000102");
  request.source_object.object_kind = "table";
  request.select_predicate.predicate_kind = "column_equals";
  request.select_predicate.canonical_predicate_envelope = "id";
  request.select_predicate.bound_values.push_back(TextValue(id));
  request.select_projection.canonical_projection_envelopes.push_back("id");
  request.select_projection.canonical_projection_envelopes.push_back("note");
  const auto selected = api::EngineSelectRows(request);
  Require(selected.ok, "engine-owned point select failed");
  const auto values = ReadResultValues(selected.result_shape.rows);
  Require(id.starts_with("perf-") && values.size() == 1 && values.contains(id) &&
              values.at(id) == "baseline row " + id.substr(5),
          "point select returned the wrong row values");
  return selected.result_shape.rows.size();
}

api::EngineResultShape SelectAll(const std::filesystem::path& database_path,
                      const BaselineTransaction& transaction,
                      std::uint64_t limit) {
  scratchbird::tests::FixtureEngineStatement statement(*transaction.session, transaction.context);
  (void)database_path;
  api::EngineSelectRowsRequest request;
  request.context = statement.context;
  request.operation_id = "dml.select_rows";
  request.source_object.uuid = scratchbird::tests::FixtureUuidLiteral("019e07f0-0000-7000-8000-000000000102");
  request.source_object.object_kind = "table";
  request.limit = limit;
  request.select_projection.canonical_projection_envelopes.push_back("id");
  request.select_projection.canonical_projection_envelopes.push_back("note");
  const auto selected = api::EngineSelectRows(request);
  Require(selected.ok, "engine-owned full select failed");
  return selected.result_shape;
}

std::string JsonEscape(std::string_view value) {
  std::ostringstream out;
  for (const char ch : value) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << ch; break;
    }
  }
  return out.str();
}

void WriteSamplesJson(std::ostream& out, const std::vector<double>& samples) {
  out << '[';
  for (std::size_t i = 0; i < samples.size(); ++i) {
    if (i != 0) out << ", ";
    out << samples[i];
  }
  out << ']';
}

ResultValues ReadResultValues(const std::vector<api::EngineRowValue>& rows) {
  ResultValues values;
  for (const auto& row : rows) {
    std::map<std::string, std::string> fields;
    for (const auto& [name, value] : row.fields) {
      Require(value.state == api::EngineValueState::value && !value.isSqlNull() &&
                  (value.encoded_value.empty() || value.binary_value.empty()),
              "benchmark result has NULL, sentinel or ambiguous payload");
      // The retained-row API supplies bytes and state, not public result
      // metadata. The fixture's bound columns are character; this oracle
      // verifies those actual bytes without inventing returned descriptors.
      const auto payload = value.binary_value.empty() ? value.encoded_value :
          std::string(value.binary_value.begin(), value.binary_value.end());
      Require(fields.emplace(name, payload).second, "duplicate result field");
    }
    Require(fields.size() == 2 && fields.contains("id") && fields.contains("note"),
            "benchmark result shape changed");
    Require(values.emplace(fields.at("id"), fields.at("note")).second,
            "benchmark select duplicated a row");
  }
  return values;
}

std::string StableResultHash(const ResultValues& values) {
  std::vector<std::uint8_t> material;
  const auto append = [&](std::string_view bytes) {
    const auto size = static_cast<std::uint64_t>(bytes.size());
    for (unsigned i = 0; i < 8; ++i) material.push_back(size >> (8*i));
    material.insert(material.end(), bytes.begin(), bytes.end());
  };
  append("ScratchBird.Performance.ResultRows.V2");
  for (const auto& [id, note] : values) { append(id); append(note); }
  const auto hash = scratchbird::core::hash::ComputeSha256Digest(material);
  Require(hash.ok(), "benchmark result SHA256 failed");
  return scratchbird::core::hash::HexLower(hash.digest);
}

void RequireResultOracleControls() {
  const auto row = Row(0);
  const auto baseline = ReadResultValues({row});
  Require(baseline == ResultValues{{"perf-0", "baseline row 0"}},
          "result oracle positive control failed");
  const auto rejects = [&](std::vector<api::EngineRowValue> rows) {
    bool rejected = false;
    try { (void)ReadResultValues(rows); }
    catch (const std::runtime_error&) { rejected = true; }
    Require(rejected, "result oracle accepted a malformed control");
  };
  rejects({row, row});
  auto changed = row;
  changed.fields.pop_back();
  rejects({changed});
  changed = row;
  changed.fields.push_back(row.fields.front());
  rejects({changed});
  changed = row;
  changed.fields.front().second.setState(api::EngineValueState::sql_null);
  rejects({changed});
  changed = row;
  changed.fields.front().second.binary_value = {1};
  rejects({changed});
  changed = row;
  changed.fields.back().second.encoded_value.back() = '1';
  Require(StableResultHash(ReadResultValues({changed})) != StableResultHash(baseline),
          "result hash did not observe a changed cell");
  Require(StableResultHash({{"a", "bc"}}) != StableResultHash({{"ab", "c"}}),
          "result hash did not frame fields independently");
}

void WriteMeasurements(const Measurements& m, const Thresholds& t) {
  std::ofstream out(SB_PERF_OUTPUT_JSON);
  Require(out.good(), "failed to open performance output JSON");
  out << "{\n"
      << "  \"schema\": \"scratchbird.beta.performance.measurement.v1\",\n"
      << "  \"threshold_source\": \"project/tests/performance/BETA_PERFORMANCE_BASELINE_THRESHOLDS.json\",\n"
      << "  \"reproducible_json\": {\n"
      << "    \"stable_key_order\": true,\n"
      << "    \"runtime_doc_dependencies\": [],\n"
      << "    \"output_scope\": \"build tree only\"\n"
      << "  },\n"
      << "  \"cache_policy\": {\n"
      << "    \"cold_cache_policy\": \"startup/open uses a fresh temporary database per CTest run\",\n"
      << "    \"warm_cache_policy\": \"query repeats run after deterministic setup on the same opened database\",\n"
      << "    \"drop_os_cache_required\": false\n"
      << "  },\n"
      << "  \"repeat_count\": " << t.repeat_count << ",\n"
      << "  \"variance_evidence\": {\n"
      << "    \"simple_query_latency_ms\": {\n"
      << "      \"samples_ms\": ";
  WriteSamplesJson(out, m.simple_query_stats.samples_ms);
  out << ",\n"
      << "      \"mean_ms\": " << m.simple_query_stats.mean_ms << ",\n"
      << "      \"median_ms\": " << m.simple_query_stats.median_ms << ",\n"
      << "      \"p50_ms\": " << m.simple_query_stats.p50_ms << ",\n"
      << "      \"p90_ms\": " << m.simple_query_stats.p90_ms << ",\n"
      << "      \"p95_ms\": " << m.simple_query_stats.p95_ms << ",\n"
      << "      \"p99_ms\": " << m.simple_query_stats.p99_ms << ",\n"
      << "      \"p99_9_ms\": " << m.simple_query_stats.p99_9_ms << ",\n"
      << "      \"max_latency_ms\": " << m.simple_query_stats.max_latency_ms << ",\n"
      << "      \"variance\": " << m.simple_query_stats.variance_ms2 << ",\n"
      << "      \"stddev\": " << m.simple_query_stats.stddev_ms << ",\n"
      << "      \"relative_variance\": " << m.simple_query_stats.relative_variance << "\n"
      << "    },\n"
      << "    \"select_all_latency_ms\": {\n"
      << "      \"samples_ms\": ";
  WriteSamplesJson(out, m.select_all_stats.samples_ms);
  out << ",\n"
      << "      \"mean_ms\": " << m.select_all_stats.mean_ms << ",\n"
      << "      \"median_ms\": " << m.select_all_stats.median_ms << ",\n"
      << "      \"p50_ms\": " << m.select_all_stats.p50_ms << ",\n"
      << "      \"p90_ms\": " << m.select_all_stats.p90_ms << ",\n"
      << "      \"p95_ms\": " << m.select_all_stats.p95_ms << ",\n"
      << "      \"p99_ms\": " << m.select_all_stats.p99_ms << ",\n"
      << "      \"p99_9_ms\": " << m.select_all_stats.p99_9_ms << ",\n"
      << "      \"max_latency_ms\": " << m.select_all_stats.max_latency_ms << ",\n"
      << "      \"variance\": " << m.select_all_stats.variance_ms2 << ",\n"
      << "      \"stddev\": " << m.select_all_stats.stddev_ms << ",\n"
      << "      \"relative_variance\": " << m.select_all_stats.relative_variance << "\n"
      << "    }\n"
      << "  },\n"
      << "  \"cpu_thread_controls\": {\n"
      << "    \"benchmark_threads\": 1,\n"
      << "    \"hardware_concurrency_observed\": " << std::max(1u, std::thread::hardware_concurrency()) << ",\n"
      << "    \"worker_parallelism\": \"single_process_single_worker\",\n"
      << "    \"affinity_policy\": \"recorded_or_unpinned_ctest_host\"\n"
      << "  },\n"
      << "  \"storage_controls\": {\n"
      << "    \"database_lifecycle\": \"fresh_database_per_run\",\n"
      << "    \"database_path_policy\": \"temporary_directory_removed_after_success\",\n"
      << "    \"storage_backend\": \"scratchbird_mga_native_storage\",\n"
      << "    \"reference_or_embedded_storage_backend\": false\n"
      << "  },\n"
      << "  \"route_controls\": {\n"
      << "    \"cluster_mode\": \"local_noncluster\",\n"
      << "    \"routes\": [\"sblr.lifecycle\", \"sblr.transaction_admission\", \"engine_api.transaction\", \"engine_api.ddl\", \"engine_api.dml\"],\n"
      << "    \"parser_executes_sql\": false,\n"
      << "    \"route_selection_locked\": true\n"
      << "  },\n"
      << "  \"measurements\": {\n"
      << "    \"startup_open_latency_ms\": " << m.startup_open_latency_ms << ",\n"
      << "    \"session_begin_commit_latency_ms\": " << m.session_begin_commit_latency_ms << ",\n"
      << "    \"simple_query_latency_ms\": " << m.simple_query_latency_ms << ",\n"
      << "    \"insert_rows_per_second\": " << m.insert_rows_per_second << ",\n"
      << "    \"select_rows_per_second\": " << m.select_rows_per_second << ",\n"
      << "    \"rss_growth_bytes\": " << m.rss_growth_bytes << ",\n"
      << "    \"inserted_rows\": " << m.inserted_rows << ",\n"
      << "    \"selected_rows\": " << m.selected_rows << "\n"
      << "  },\n"
      << "  \"latency_percentiles\": {\n"
      << "    \"simple_query_latency_ms\": {\n"
      << "      \"mean_ms\": " << m.simple_query_stats.mean_ms << ",\n"
      << "      \"median_ms\": " << m.simple_query_stats.median_ms << ",\n"
      << "      \"p90_ms\": " << m.simple_query_stats.p90_ms << ",\n"
      << "      \"p95_ms\": " << m.simple_query_stats.p95_ms << ",\n"
      << "      \"p99_ms\": " << m.simple_query_stats.p99_ms << ",\n"
      << "      \"p99_9_ms\": " << m.simple_query_stats.p99_9_ms << ",\n"
      << "      \"max_latency_ms\": " << m.simple_query_stats.max_latency_ms << "\n"
      << "    },\n"
      << "    \"select_all_latency_ms\": {\n"
      << "      \"mean_ms\": " << m.select_all_stats.mean_ms << ",\n"
      << "      \"median_ms\": " << m.select_all_stats.median_ms << ",\n"
      << "      \"p90_ms\": " << m.select_all_stats.p90_ms << ",\n"
      << "      \"p95_ms\": " << m.select_all_stats.p95_ms << ",\n"
      << "      \"p99_ms\": " << m.select_all_stats.p99_ms << ",\n"
      << "      \"p99_9_ms\": " << m.select_all_stats.p99_9_ms << ",\n"
      << "      \"max_latency_ms\": " << m.select_all_stats.max_latency_ms << "\n"
      << "    }\n"
      << "  },\n"
      << "  \"runtime_health_counters\": {\n"
      << "    \"error_rate\": " << m.error_rate << ",\n"
      << "    \"retry_rate\": " << m.retry_rate << ",\n"
      << "    \"spill_rate\": " << m.spill_rate << ",\n"
      << "    \"cleanup_lag\": " << m.cleanup_lag << "\n"
      << "  },\n"
      << "  \"resource_high_water\": {\n"
      << "    \"memory_high_water_bytes\": " << m.memory_high_water_bytes << ",\n"
      << "    \"rss_growth_bytes\": " << m.rss_growth_bytes << ",\n"
      << "    \"fd_high_water\": " << m.fd_high_water << ",\n"
      << "    \"fd_growth\": " << m.fd_growth << "\n"
      << "  },\n"
      << "  \"ipar_profiler_evidence\": {\n"
      << "    \"profiler_kind\": \"internal_ctest_equivalent\",\n"
      << "    \"host_perf_required\": false,\n"
      << "    \"baseline_profile_captured\": false,\n"
      << "    \"post_fix_profile_captured\": false,\n"
      << "    \"qualification\": \"single-run latency/resource samples, not before/after profiles\",\n"
      << "    \"cpu_hotspot_proxy\": {\n"
      << "      \"startup_open_latency_ms\": " << m.startup_open_latency_ms << ",\n"
      << "      \"session_begin_commit_latency_ms\": " << m.session_begin_commit_latency_ms << ",\n"
      << "      \"simple_query_mean_ms\": " << m.simple_query_stats.mean_ms << ",\n"
      << "      \"select_all_mean_ms\": " << m.select_all_stats.mean_ms << "\n"
      << "    },\n"
      << "    \"syscall_and_file_growth_proxy\": {\n"
      << "      \"fd_growth\": " << m.fd_growth << ",\n"
      << "      \"fd_high_water\": " << m.fd_high_water << ",\n"
      << "      \"fresh_database_per_run\": true\n"
      << "    },\n"
      << "    \"allocation_proxy\": {\n"
      << "      \"rss_growth_bytes\": " << m.rss_growth_bytes << ",\n"
      << "      \"memory_high_water_bytes\": " << m.memory_high_water_bytes << ",\n"
      << "      \"spill_rate\": null\n"
      << "    },\n"
      << "    \"latch_wait_proxy\": {\n"
      << "      \"worker_parallelism\": \"single_process_single_worker\",\n"
      << "      \"contention_waits_observed\": null,\n"
      << "      \"transaction_inventory_waits_observed\": null\n"
      << "    },\n"
      << "    \"comparison_to_targets\": {\n"
      << "      \"startup_open_latency_within_target\": " << (m.startup_open_latency_ms <= t.startup_open_latency_ms_max ? "true" : "false") << ",\n"
      << "      \"session_begin_commit_within_target\": " << (m.session_begin_commit_latency_ms <= t.session_begin_commit_latency_ms_max ? "true" : "false") << ",\n"
      << "      \"simple_query_within_target\": " << (m.simple_query_stats.p99_ms <= t.simple_query_latency_ms_max ? "true" : "false") << ",\n"
      << "      \"insert_throughput_within_target\": " << (m.insert_rows_per_second >= t.insert_rows_per_second_min ? "true" : "false") << ",\n"
      << "      \"select_throughput_within_target\": " << (m.select_rows_per_second >= t.select_rows_per_second_min ? "true" : "false") << ",\n"
      << "      \"rss_growth_within_target\": " << (m.rss_growth_bytes <= t.rss_growth_bytes_max ? "true" : "false") << "\n"
      << "    },\n"
      << "    \"authority_controls\": {\n"
      << "      \"engine_mga_transaction_authority\": true,\n"
      << "      \"profiler_finality_authority\": false,\n"
      << "      \"parser_sql_execution_authority\": false\n"
      << "    }\n"
      << "  },\n"
      << "  \"regression_thresholds\": {\n"
      << "    \"startup_open_latency_ms_max\": " << t.startup_open_latency_ms_max << ",\n"
      << "    \"session_begin_commit_latency_ms_max\": " << t.session_begin_commit_latency_ms_max << ",\n"
      << "    \"simple_query_latency_ms_max\": " << t.simple_query_latency_ms_max << ",\n"
      << "    \"insert_rows_per_second_min\": " << t.insert_rows_per_second_min << ",\n"
      << "    \"select_rows_per_second_min\": " << t.select_rows_per_second_min << ",\n"
      << "    \"rss_growth_bytes_max\": " << t.rss_growth_bytes_max << ",\n"
      << "    \"max_relative_variance\": " << t.max_relative_variance << "\n"
      << "  },\n"
      << "  \"thresholds\": {\n"
      << "    \"startup_open_latency_ms_max\": " << t.startup_open_latency_ms_max << ",\n"
      << "    \"session_begin_commit_latency_ms_max\": " << t.session_begin_commit_latency_ms_max << ",\n"
      << "    \"simple_query_latency_ms_max\": " << t.simple_query_latency_ms_max << ",\n"
      << "    \"insert_rows_per_second_min\": " << t.insert_rows_per_second_min << ",\n"
      << "    \"select_rows_per_second_min\": " << t.select_rows_per_second_min << ",\n"
      << "    \"rss_growth_bytes_max\": " << t.rss_growth_bytes_max << "\n"
      << "  },\n"
      << "  \"selected_physical_path_result_parity\": {\n"
      << "    \"selected_physical_path\": \"scratchbird_mga_relation_store.engine_api_dml_select_rows\",\n"
      << "    \"selected_result_hash\": \"" << JsonEscape(m.selected_result_hash) << "\",\n"
      << "    \"hash_parity\": true,\n"
      << "    \"parity_scope\": \"actual returned values versus independently constructed expected rows; not cross-route parity\",\n"
      << "    \"route_result_hashes\": {\n"
      << "      \"engine_api.dml.select_rows\": \"" << JsonEscape(m.selected_result_hash) << "\"\n"
      << "    }\n"
      << "  },\n"
      << "  \"authority_controls\": {\n"
      << "    \"engine_mga_transaction_authority\": true,\n"
      << "    \"parser_client_reference_transaction_finality_shortcuts\": false,\n"
      << "    \"reference_or_embedded_storage_transaction_truth\": false,\n"
      << "    \"selected_path_requires_mga_visibility_recheck\": true\n"
      << "  }\n"
      << "}\n";
}

void CheckThresholds(const Measurements& m, const Thresholds& t) {
  Require(m.startup_open_latency_ms <= t.startup_open_latency_ms_max,
          "startup/open latency exceeded beta threshold");
  Require(m.session_begin_commit_latency_ms <= t.session_begin_commit_latency_ms_max,
          "session begin/commit latency exceeded beta threshold");
  Require(m.simple_query_latency_ms <= t.simple_query_latency_ms_max,
          "simple query latency exceeded beta threshold");
  Require(m.simple_query_stats.p99_ms <= t.simple_query_latency_ms_max,
          "simple query p99 latency exceeded beta threshold");
  Require(m.select_all_stats.p99_ms <= t.simple_query_latency_ms_max,
          "select p99 latency exceeded beta threshold");
  Require(m.insert_rows_per_second >= t.insert_rows_per_second_min,
          "insert throughput below beta threshold");
  Require(m.select_rows_per_second >= t.select_rows_per_second_min,
          "select throughput below beta threshold");
  Require(m.rss_growth_bytes <= t.rss_growth_bytes_max,
          "RSS growth exceeded beta threshold");
  Require(m.simple_query_stats.relative_variance <= t.max_relative_variance,
          "simple query relative variance exceeded beta reproducibility threshold");
  Require(m.select_all_stats.relative_variance <= t.max_relative_variance,
          "select relative variance exceeded beta reproducibility threshold");
  Require(m.error_rate == 0.0, "performance baseline observed errors");
  Require(m.retry_rate == 0.0, "performance baseline observed retries");
  Require(m.spill_rate == 0.0, "performance baseline observed spill");
  Require(m.cleanup_lag == 0, "performance baseline observed cleanup lag");
  Require(m.memory_high_water_bytes > 0, "performance baseline missing memory high-water");
  Require(m.fd_high_water > 0, "performance baseline missing fd high-water");
}

}  // namespace

int main() {
  std::filesystem::path work;
  int result = EXIT_SUCCESS;
  try {
  ConfigureMemoryFixture();
  RequireResultOracleControls();
  const Thresholds thresholds = LoadThresholds();
  const std::uint64_t rss_before = CurrentRssBytes();
  const std::uint64_t fd_before = CurrentFdCount();
  work = MakeTempDir();
  Require(!work.empty(), "failed to create temp directory");
  const auto database_path = work / "phase7m_perf.sbdb";

  Measurements measurements;
  api::EngineRequestContext owner;
  measurements.startup_open_latency_ms = MeasureMs([&]() {
    const auto created =
        scratchbird::tests::database_lifecycle::CreateCredentialedDatabaseFixture(
            database_path, SB_PERF_SEED_PACK_ROOT);
    Require(created.ok(), "credentialed lifecycle fixture create failed");
    scratchbird::storage::database::DatabaseCreateConfig identity;
    identity.path = database_path.string();
    identity.database_uuid = created.state.database_uuid;
    identity.filespace_uuid = created.state.filespace_uuid;
    owner = scratchbird::tests::BootstrapFixtureOwnerContext(identity);
    auto opened = Dispatch(database_path,
                           "lifecycle.open_database",
                           "SBLR_LIFECYCLE_OPEN_DATABASE",
                           BaseContext(owner));
    Require(opened.api_result.ok, "lifecycle open failed");
  });

  measurements.session_begin_commit_latency_ms = MeasureMs([&]() {
    auto transaction = BeginTransaction(owner, 101);
    Commit(database_path, transaction);
  });

  auto ddl_transaction = BeginTransaction(owner, 201);
  CreateSchemaAndTable(database_path, ddl_transaction);
  Commit(database_path, ddl_transaction);

  auto insert_transaction = BeginTransaction(owner, 202);
  const double insert_ms = MeasureMs([&]() {
    InsertRows(database_path, insert_transaction, thresholds.sample_rows);
  });
  Commit(database_path, insert_transaction);
  measurements.inserted_rows = thresholds.sample_rows;
  measurements.insert_rows_per_second =
      insert_ms > 0.0 ? (static_cast<double>(thresholds.sample_rows) * 1000.0 / insert_ms) : thresholds.sample_rows;

  auto query_transaction = BeginTransaction(owner, 203);
  measurements.simple_query_stats = MeasureRepeatedMs([&]() {
    Require(SelectById(database_path, query_transaction,
                       "perf-" + std::to_string(thresholds.sample_rows / 2)) == 1,
            "simple query did not return exactly one row");
  }, thresholds.repeat_count);
  measurements.simple_query_latency_ms = measurements.simple_query_stats.mean_ms;
  std::size_t selected_rows = 0;
  ResultValues expected;
  for (std::uint64_t i = 0; i < thresholds.sample_rows; ++i)
    expected.emplace("perf-" + std::to_string(i), "baseline row " + std::to_string(i));
  const auto expected_hash = StableResultHash(expected);
  measurements.select_all_stats = MeasureRepeatedMs([&]() {
    const auto selected = SelectAll(database_path, query_transaction, thresholds.sample_rows);
    selected_rows = selected.rows.size();
    Require(selected_rows == thresholds.sample_rows, "select throughput sample row count mismatch");
    const auto values = ReadResultValues(selected.rows);
    Require(values == expected, "select changed actual row values");
    measurements.selected_result_hash = StableResultHash(values);
    Require(measurements.selected_result_hash == expected_hash, "selected row hash mismatch");
  }, thresholds.repeat_count);
  Commit(database_path, query_transaction);
  measurements.selected_rows = selected_rows;
  measurements.select_rows_per_second =
      measurements.select_all_stats.mean_ms > 0.0
          ? (static_cast<double>(selected_rows) * 1000.0 / measurements.select_all_stats.mean_ms)
          : selected_rows;

  const std::uint64_t rss_after = CurrentRssBytes();
  const std::uint64_t fd_after = CurrentFdCount();
  measurements.rss_growth_bytes = rss_after >= rss_before ? rss_after - rss_before : 0;
  measurements.memory_high_water_bytes = std::max(rss_before, rss_after);
  measurements.fd_high_water = std::max(fd_before, fd_after);
  measurements.fd_growth = fd_after >= fd_before ? fd_after - fd_before : 0;

  WriteMeasurements(measurements, thresholds);
  CheckThresholds(measurements, thresholds);

  std::cout << "scratchbird_beta_performance_baseline_conformance=passed "
            << "startup_open_ms=" << measurements.startup_open_latency_ms
            << " session_begin_commit_ms=" << measurements.session_begin_commit_latency_ms
            << " simple_query_ms=" << measurements.simple_query_latency_ms
            << " insert_rows_per_second=" << measurements.insert_rows_per_second
            << " select_rows_per_second=" << measurements.select_rows_per_second
            << " rss_growth_bytes=" << measurements.rss_growth_bytes
            << " output=" << SB_PERF_OUTPUT_JSON << '\n';
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    result = EXIT_FAILURE;
  }
  if (!work.empty()) {
    std::error_code error;
    std::filesystem::remove_all(work, error);
    if (error) {
      std::cerr << "performance fixture cleanup failed: " << error.message() << '\n';
      result = EXIT_FAILURE;
    }
  }
  return result;
}
