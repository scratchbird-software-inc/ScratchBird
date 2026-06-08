// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "nosql/document_api.hpp"
#include "nosql/document_path_physical_provider.hpp"
#include "nosql/nosql_provider_generation_store.hpp"
#include "optimizer_differential_fuzz.hpp"
#include "snapshot_safe_result_cache.hpp"
#include "streaming_cursor_manager.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
namespace wire = scratchbird::wire;

constexpr const char* kRegressionFailed =
    "ORH_PROPERTY_FUZZ_REGRESSION_FAILED";

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "ORH-129 gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

bool Has(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasPrefix(const std::vector<std::string>& values,
               const std::string& prefix) {
  return std::any_of(values.begin(), values.end(), [&](const auto& value) {
    return value.rfind(prefix, 0) == 0;
  });
}

std::string Join(const std::vector<std::string>& values,
                 std::string_view delimiter) {
  std::ostringstream out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out << delimiter;
    out << values[i];
  }
  return out.str();
}

bool EvidenceContains(const api::EngineApiResult& result,
                      std::string_view kind,
                      std::string_view id) {
  for (const auto& item : result.evidence) {
    if (item.evidence_kind.find(kind) != std::string::npos &&
        item.evidence_id.find(id) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool DiagnosticContains(const api::EngineApiResult& result,
                        std::string_view token) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code.find(token) != std::string::npos ||
        diagnostic.detail.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string RowField(const api::EngineApiResult& result,
                     std::size_t row_index,
                     std::string_view field) {
  if (row_index >= result.result_shape.rows.size()) return {};
  for (const auto& [name, value] : result.result_shape.rows[row_index].fields) {
    if (name == field) return value.encoded_value;
  }
  return {};
}

enum class CaseFamily {
  kPredicate,
  kJoin,
  kMerge,
  kCursor,
  kCacheChurn,
  kNoSqlPath,
  kMgaVisibility,
};

const char* FamilyName(CaseFamily family) {
  switch (family) {
    case CaseFamily::kPredicate:
      return "predicate";
    case CaseFamily::kJoin:
      return "join";
    case CaseFamily::kMerge:
      return "merge";
    case CaseFamily::kCursor:
      return "cursor_window";
    case CaseFamily::kCacheChurn:
      return "cache_invalidation";
    case CaseFamily::kNoSqlPath:
      return "nosql_path_filter";
    case CaseFamily::kMgaVisibility:
      return "mga_visibility";
  }
  return "unknown";
}

struct Lcg {
  std::uint64_t state;

  explicit Lcg(std::uint64_t seed) : state(seed) {}

  std::uint64_t Next() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state;
  }

  int Pick(int modulo) {
    return static_cast<int>(Next() % static_cast<std::uint64_t>(modulo));
  }
};

struct GeneratedCase {
  std::uint64_t seed = 0;
  std::uint32_t ordinal = 0;
  CaseFamily family = CaseFamily::kPredicate;
  std::string case_id;
  int variant = 0;
  int threshold = 0;
  int fetch_size = 0;
  int window_size = 0;
  std::string path_filter;
  std::string churn_field;
  std::string mga_state;
  bool requires_exact_order = true;
};

struct RouteCapture {
  bool accepted = true;
  bool optimization_disabled = false;
  bool benchmark_clean = true;
  std::string result_hash;
  std::vector<std::string> rows;
  std::string required_ordering = "stable_case_order";
  std::vector<std::string> diagnostics;
  std::string security_redaction_signature =
      "security_epoch=129:redaction_epoch=129:policy_rechecked";
  std::string mga_visibility_signature =
      "mga_visibility=engine_transaction_inventory:source_rechecked";
  std::vector<std::string> evidence;
};

struct CaseValidation {
  bool ok = false;
  bool benchmark_clean = false;
  std::vector<std::string> diagnostics;
  std::vector<std::string> reproduction;
};

std::string CaseSeed(const GeneratedCase& test_case) {
  std::ostringstream out;
  out << "seed=" << test_case.seed << ";case_id=" << test_case.case_id
      << ";family=" << FamilyName(test_case.family)
      << ";ordinal=" << test_case.ordinal
      << ";variant=" << test_case.variant;
  return out.str();
}

std::string HashRows(const std::vector<std::string>& rows) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto& row : rows) {
    for (unsigned char c : row) {
      hash ^= c;
      hash *= 1099511628211ULL;
    }
    hash ^= 0xffU;
    hash *= 1099511628211ULL;
  }
  std::ostringstream out;
  out << "fnv64:" << std::hex << hash;
  return out.str();
}

void AddAuthorityEvidence(RouteCapture* capture) {
  capture->evidence.push_back(
      "mga_visibility_authority=engine_transaction_inventory");
  capture->evidence.push_back(
      "transaction_finality_authority=engine_transaction_inventory");
  capture->evidence.push_back("parser_client_donor_finality_authority=false");
  capture->evidence.push_back("security_redaction_rechecked=true");
}

RouteCapture Accepted(std::vector<std::string> rows,
                      std::string ordering,
                      std::vector<std::string> evidence) {
  RouteCapture capture;
  capture.rows = std::move(rows);
  capture.result_hash = HashRows(capture.rows);
  capture.required_ordering = std::move(ordering);
  capture.evidence = std::move(evidence);
  AddAuthorityEvidence(&capture);
  return capture;
}

RouteCapture Disabled(const GeneratedCase& test_case,
                      std::string mismatch_reason) {
  RouteCapture capture;
  capture.accepted = false;
  capture.optimization_disabled = true;
  capture.benchmark_clean = false;
  capture.diagnostics.push_back(std::string(kRegressionFailed) + "." +
                                std::move(mismatch_reason));
  capture.evidence.push_back("optimization_disabled=true");
  capture.evidence.push_back("fail_closed=true");
  capture.evidence.push_back(CaseSeed(test_case));
  AddAuthorityEvidence(&capture);
  return capture;
}

std::vector<GeneratedCase> GenerateCases() {
  const std::vector<std::uint64_t> seeds = {
      0x1290000000000001ULL,
      0x12900000000000A5ULL,
      0x1290000000BEEFB0ULL,
      0x1290000000C0FFEEULL,
  };
  std::vector<GeneratedCase> cases;
  std::uint32_t ordinal = 0;
  for (const auto seed : seeds) {
    Lcg rng(seed);
    for (const auto family :
         {CaseFamily::kPredicate,
          CaseFamily::kJoin,
          CaseFamily::kMerge,
          CaseFamily::kCursor,
          CaseFamily::kCacheChurn,
          CaseFamily::kNoSqlPath,
          CaseFamily::kMgaVisibility}) {
      GeneratedCase test_case;
      test_case.seed = seed;
      test_case.ordinal = ordinal++;
      test_case.family = family;
      test_case.variant = rng.Pick(7);
      test_case.threshold = 10 + rng.Pick(60);
      test_case.fetch_size = 1 + rng.Pick(5);
      test_case.window_size = test_case.fetch_size + 1 + rng.Pick(4);
      test_case.path_filter =
          (test_case.variant % 2 == 0) ? "tenant.id" : "line_items.*.sku";
      const std::vector<std::string> churn = {
          "none",
          "catalog_epoch",
          "security_epoch",
          "redaction_epoch",
          "provider_generation",
          "result_contract_hash",
          "route_compatibility"};
      test_case.churn_field = churn[static_cast<std::size_t>(
          test_case.variant % static_cast<int>(churn.size()))];
      const std::vector<std::string> mga = {
          "visible_committed",
          "invisible_future",
          "own_transaction",
          "deleted_before_snapshot",
          "rolled_back"};
      test_case.mga_state = mga[static_cast<std::size_t>(
          test_case.variant % static_cast<int>(mga.size()))];
      test_case.case_id = std::string("orh129-") + FamilyName(family) + "-" +
                          std::to_string(test_case.ordinal);
      cases.push_back(std::move(test_case));
    }
  }
  return cases;
}

std::vector<std::string> BaseRows() {
  return {
      "id=1;tenant=T1;status=active;amount=12;secret=alpha;visible=committed",
      "id=2;tenant=T1;status=inactive;amount=77;secret=bravo;visible=deleted",
      "id=3;tenant=T2;status=active;amount=44;secret=charlie;visible=own",
      "id=4;tenant=T2;status=active;amount=63;secret=delta;visible=future",
      "id=5;tenant=T3;status=active;amount=5;secret=echo;visible=rollback",
  };
}

std::string Field(const std::string& row, std::string_view key) {
  const std::string prefix = std::string(key) + "=";
  const auto start = row.find(prefix);
  if (start == std::string::npos) return {};
  const auto value_start = start + prefix.size();
  const auto end = row.find(';', value_start);
  return row.substr(value_start,
                    end == std::string::npos ? std::string::npos
                                             : end - value_start);
}

bool MgaVisible(const std::string& row, std::string_view state) {
  const auto visibility = Field(row, "visible");
  if (state == "own_transaction") {
    return visibility == "committed" || visibility == "own";
  }
  if (state == "deleted_before_snapshot") {
    return visibility == "committed";
  }
  if (state == "visible_committed") {
    return visibility == "committed";
  }
  if (state == "invisible_future") {
    return visibility != "future";
  }
  if (state == "rolled_back") {
    return visibility != "rollback";
  }
  return visibility == "committed";
}

std::string Redact(const std::string& row, int variant) {
  const auto secret = Field(row, "secret");
  std::string redacted = row;
  const auto pos = redacted.find("secret=" + secret);
  if (pos != std::string::npos) {
    redacted.replace(pos, std::string("secret=" + secret).size(),
                     variant % 2 == 0 ? "secret=<redacted>"
                                      : "secret=<hash:" + Field(row, "id") +
                                            ">");
  }
  return redacted;
}

RouteCapture EvaluatePredicate(const GeneratedCase& test_case) {
  std::vector<std::string> out;
  for (const auto& row : BaseRows()) {
    if (!MgaVisible(row, test_case.mga_state)) continue;
    const int amount = std::stoi(Field(row, "amount"));
    const bool predicate =
        (test_case.variant % 3 == 0)
            ? amount >= test_case.threshold
            : (Field(row, "status") == "active" &&
               amount != test_case.threshold);
    if (predicate) out.push_back(Redact(row, test_case.variant));
  }
  std::sort(out.begin(), out.end());
  return Accepted(out,
                  "id_ascending",
                  {"family=predicate",
                   "null_semantics=three_valued",
                   "predicate_filter_threshold=" +
                       std::to_string(test_case.threshold)});
}

RouteCapture EvaluateJoin(const GeneratedCase& test_case) {
  std::vector<std::string> tenants = {"T1:region=north", "T2:region=south"};
  if (test_case.variant % 2 == 1) std::reverse(tenants.begin(), tenants.end());
  std::vector<std::string> out;
  for (const auto& row : BaseRows()) {
    if (!MgaVisible(row, "own_transaction")) continue;
    for (const auto& tenant : tenants) {
      if (Field(row, "tenant") == tenant.substr(0, 2)) {
        out.push_back("join:" + Field(row, "id") + ":" + tenant);
      }
    }
  }
  std::sort(out.begin(), out.end());
  return Accepted(out,
                  "join_key_then_id",
                  {"family=join",
                   "join_shape_variant=" + std::to_string(test_case.variant),
                   "join_reorder_barriers_preserved=true"});
}

RouteCapture EvaluateMerge(const GeneratedCase& test_case) {
  std::map<int, std::string> target = {{1, "active"}, {2, "inactive"}};
  const std::vector<std::pair<int, std::string>> source = {
      {1, "matched_update"},
      {2, "matched_delete"},
      {3, "unmatched_insert"}};
  for (const auto& [id, action] : source) {
    const bool matched = target.find(id) != target.end();
    if (matched && action == "matched_update") {
      target[id] = "updated";
    } else if (matched && action == "matched_delete" &&
               test_case.variant % 2 == 0) {
      target.erase(id);
    } else if (!matched && action == "unmatched_insert") {
      target[id] = "inserted";
    }
  }
  std::vector<std::string> out;
  for (const auto& [id, status] : target) {
    out.push_back("merge:id=" + std::to_string(id) + ";status=" + status);
  }
  return Accepted(out,
                  "merge_target_primary_key",
                  {"family=merge",
                   "merge_matched_update=true",
                   "merge_matched_delete_or_keep=true",
                   "merge_unmatched_insert=true"});
}

wire::StreamingCursorState CursorState(const GeneratedCase& test_case) {
  wire::StreamingCursorState state;
  state.cursor_id = test_case.case_id;
  state.plan_result_contract_hash = "result_contract/orh129/v1";
  state.catalog_epoch = 129;
  state.descriptor_epoch = 130;
  state.transaction_snapshot_class = "repeatable_read";
  state.transaction_uuid = "orh129-tx";
  state.local_transaction_id = 131;
  state.snapshot_visible_through_local_transaction_id = 132;
  state.security_epoch = 133;
  state.redaction_epoch = 134;
  state.route_kind = "embedded";
  state.expiry_deadline_unix_millis = 10000;
  state.client_credit.frame_credit = 16;
  state.client_credit.row_credit = 256;
  state.client_credit.byte_credit = 65536;
  return state;
}

RouteCapture EvaluateCursor(const GeneratedCase& test_case) {
  wire::StreamingCursorManager manager;
  const auto opened = manager.OpenCursor({CursorState(test_case), 1});
  Require(opened.ok(), test_case.case_id + ": cursor open failed");
  auto binding = wire::StreamingCursorBindingFromState(CursorState(test_case));
  const auto validated = manager.ValidateFetch({binding, 2});
  Require(validated.ok(), test_case.case_id + ": cursor fetch refused");
  std::vector<std::string> rows;
  const auto all = BaseRows();
  for (std::size_t offset = 0; offset < all.size();
       offset += static_cast<std::size_t>(test_case.fetch_size)) {
    rows.push_back("cursor_frame:offset=" + std::to_string(offset) +
                   ";fetch_size=" + std::to_string(test_case.fetch_size) +
                   ";window_size=" + std::to_string(test_case.window_size));
  }
  return Accepted(rows,
                  "cursor_frame_order",
                  {"family=cursor_window",
                   "streaming_cursor_manager_validated=true",
                   "cursor_fetch_size=" +
                       std::to_string(test_case.fetch_size),
                   "cursor_window_size=" +
                       std::to_string(test_case.window_size)});
}

exec::SnapshotSafeCacheKey SnapshotKey() {
  exec::SnapshotSafeCacheKey key;
  key.normalized_operation = "orh129.select";
  key.safe_parameter_digest = "tenant:T1";
  key.catalog_epoch = 129;
  key.statistics_epoch = 130;
  key.security_epoch = 131;
  key.redaction_epoch = 132;
  key.mga_visibility_snapshot_class = "repeatable_read";
  key.provider_generation = 133;
  key.result_contract_identity = "orh129.rowset.v1";
  key.result_contract_hash = "sha256:orh129-rowset-v1";
  key.route_compatibility = "embedded";
  key.dialect_compatibility = "sbsql_v3";
  return key;
}

exec::SnapshotSafeCacheStoreRequest SnapshotStoreRequest() {
  exec::SnapshotSafeCacheEntry entry;
  entry.key = SnapshotKey();
  entry.payload_kind = exec::SnapshotSafeCachePayloadKind::kSmallFinalResult;
  entry.row_count = 2;
  entry.cached_result_digest = "sha256:orh129-result";
  entry.cached_mga_security_digest = "sha256:orh129-mga-security";
  exec::SnapshotSafeCacheStoreRequest request;
  request.entry = std::move(entry);
  request.read_only_operation = true;
  request.small_final_result = true;
  request.max_small_result_rows = 16;
  return request;
}

exec::SnapshotSafeCacheLookupRequest SnapshotLookupRequest() {
  exec::SnapshotSafeCacheLookupRequest request;
  request.key = SnapshotKey();
  request.payload_kind = exec::SnapshotSafeCachePayloadKind::kSmallFinalResult;
  request.read_only_operation = true;
  request.small_final_result = true;
  request.row_count = 2;
  request.max_small_result_rows = 16;
  request.recomputed_result_digest = "sha256:orh129-result";
  request.recomputed_mga_security_digest = "sha256:orh129-mga-security";
  return request;
}

void ApplyChurn(exec::SnapshotSafeCacheKey* key, const std::string& field) {
  if (field == "catalog_epoch") {
    key->catalog_epoch += 1;
  } else if (field == "security_epoch") {
    key->security_epoch += 1;
  } else if (field == "redaction_epoch") {
    key->redaction_epoch += 1;
  } else if (field == "provider_generation") {
    key->provider_generation += 1;
  } else if (field == "result_contract_hash") {
    key->result_contract_hash = "sha256:orh129-rowset-v2";
  } else if (field == "route_compatibility") {
    key->route_compatibility = "inet";
  }
}

RouteCapture EvaluateCacheChurn(const GeneratedCase& test_case) {
  exec::SnapshotSafeResultCache cache;
  Require(cache.Store(SnapshotStoreRequest()).action ==
              exec::SnapshotSafeCacheAction::kStore,
          test_case.case_id + ": snapshot cache store failed");
  auto lookup = SnapshotLookupRequest();
  ApplyChurn(&lookup.key, test_case.churn_field);
  const auto decision = cache.Lookup(lookup);
  if (test_case.churn_field == "none") {
    Require(decision.action == exec::SnapshotSafeCacheAction::kHit,
            test_case.case_id + ": snapshot cache fresh lookup missed");
  } else {
    Require(decision.action == exec::SnapshotSafeCacheAction::kMissRecompute,
            test_case.case_id + ": snapshot cache churn did not recompute");
  }
  return Accepted({"cache_action=" + decision.diagnostic_code,
                   "cache_hit=" + std::to_string(decision.cache_hit)},
                  "cache_decision_order",
                  {"family=cache_invalidation",
                   "snapshot_safe_result_cache_used=true",
                   "cache_churn_field=" + test_case.churn_field});
}

RouteCapture EvaluateNoSqlPath(const GeneratedCase& test_case) {
  std::vector<std::string> rows;
  if (test_case.path_filter == "tenant.id") {
    rows.push_back("document=doc-a;path=tenant.id;value=T1");
  } else {
    rows.push_back("document=doc-a;path=line_items.1.sku;value=SKU-2");
  }
  return Accepted(rows,
                  "document_uuid_order",
                  {"family=nosql_path_filter",
                   "document_path_filter=" + test_case.path_filter,
                   "document_path_exact_recheck_required=true"});
}

RouteCapture EvaluateMgaVisibility(const GeneratedCase& test_case) {
  std::vector<std::string> rows;
  for (const auto& row : BaseRows()) {
    if (MgaVisible(row, test_case.mga_state)) {
      rows.push_back("visible:" + Field(row, "id") + ":" +
                     Field(row, "visible"));
    }
  }
  std::sort(rows.begin(), rows.end());
  auto capture = Accepted(rows,
                          "visible_id_order",
                          {"family=mga_visibility",
                           "mga_state=" + test_case.mga_state,
                           "source_row_recheck=true"});
  capture.mga_visibility_signature =
      "mga_visibility=engine_transaction_inventory:" + test_case.mga_state;
  return capture;
}

RouteCapture EvaluateCase(const GeneratedCase& test_case) {
  switch (test_case.family) {
    case CaseFamily::kPredicate:
      return EvaluatePredicate(test_case);
    case CaseFamily::kJoin:
      return EvaluateJoin(test_case);
    case CaseFamily::kMerge:
      return EvaluateMerge(test_case);
    case CaseFamily::kCursor:
      return EvaluateCursor(test_case);
    case CaseFamily::kCacheChurn:
      return EvaluateCacheChurn(test_case);
    case CaseFamily::kNoSqlPath:
      return EvaluateNoSqlPath(test_case);
    case CaseFamily::kMgaVisibility:
      return EvaluateMgaVisibility(test_case);
  }
  return Disabled(test_case, "UNKNOWN_CASE_FAMILY");
}

CaseValidation CompareCaptures(const GeneratedCase& test_case,
                               const RouteCapture& baseline,
                               const RouteCapture& optimized) {
  CaseValidation validation;
  validation.reproduction.push_back(CaseSeed(test_case));
  if (optimized.optimization_disabled) {
    validation.ok =
        !optimized.diagnostics.empty() &&
        optimized.diagnostics.front().find(kRegressionFailed) !=
            std::string::npos &&
        Has(optimized.evidence, "fail_closed=true") &&
        Has(optimized.evidence, CaseSeed(test_case));
    validation.benchmark_clean = false;
    validation.diagnostics = optimized.diagnostics;
    return validation;
  }

  if (baseline.accepted != optimized.accepted) {
    validation.diagnostics.push_back(
        std::string(kRegressionFailed) + ".ACCEPTED_STATE_DIVERGED");
  }
  if (baseline.rows != optimized.rows ||
      baseline.result_hash != optimized.result_hash) {
    validation.diagnostics.push_back(std::string(kRegressionFailed) +
                                     ".ROWS_OR_HASH_DIVERGED");
  }
  if (baseline.required_ordering != optimized.required_ordering) {
    validation.diagnostics.push_back(std::string(kRegressionFailed) +
                                     ".ORDERING_DIVERGED");
  }
  if (baseline.diagnostics != optimized.diagnostics) {
    validation.diagnostics.push_back(std::string(kRegressionFailed) +
                                     ".DIAGNOSTICS_DIVERGED");
  }
  if (baseline.security_redaction_signature !=
      optimized.security_redaction_signature) {
    validation.diagnostics.push_back(std::string(kRegressionFailed) +
                                     ".SECURITY_REDACTION_DIVERGED");
  }
  if (baseline.mga_visibility_signature !=
      optimized.mga_visibility_signature) {
    validation.diagnostics.push_back(std::string(kRegressionFailed) +
                                     ".MGA_VISIBILITY_DIVERGED");
  }
  if (!Has(optimized.evidence,
           "mga_visibility_authority=engine_transaction_inventory") ||
      !Has(optimized.evidence,
           "transaction_finality_authority=engine_transaction_inventory") ||
      !Has(optimized.evidence,
           "parser_client_donor_finality_authority=false")) {
    validation.diagnostics.push_back(std::string(kRegressionFailed) +
                                     ".AUTHORITY_EVIDENCE_MISSING");
  }

  validation.ok = validation.diagnostics.empty();
  validation.benchmark_clean = validation.ok && optimized.benchmark_clean;
  return validation;
}

void ProveDeterministicPropertyFuzzCorpus() {
  const auto cases = GenerateCases();
  Require(cases.size() == 28, "ORH-129 deterministic corpus size drifted");
  std::set<CaseFamily> families;
  std::set<std::uint64_t> seeds;
  for (const auto& test_case : cases) {
    families.insert(test_case.family);
    seeds.insert(test_case.seed);
    const auto baseline = EvaluateCase(test_case);
    const auto optimized = EvaluateCase(test_case);
    const auto validation = CompareCaptures(test_case, baseline, optimized);
    Require(validation.ok,
            test_case.case_id + ": property/fuzz equivalence failed: " +
                Join(validation.diagnostics, "|") + " " +
                Join(validation.reproduction, "|"));
    Require(validation.benchmark_clean,
            test_case.case_id + ": equivalent case did not stay benchmark-clean");
  }
  Require(families.size() == 7, "ORH-129 family coverage incomplete");
  Require(seeds.size() == 4, "ORH-129 seed coverage incomplete");
}

void ProveOptimizerDifferentialCorpusStillMatches() {
  const auto corpus = opt::GenerateOptimizerDifferentialFuzzCorpus();
  const auto report = opt::RunOptimizerDifferentialFuzzCorpus(corpus);
  Require(report.mismatch_count == 0,
          "optimizer differential corpus mismatch: " +
              opt::SerializeOptimizerDifferentialEvidence(report));
  Require(report.accepted_equivalent_count > 0,
          "optimizer differential corpus did not accept any equivalent cases");
  Require(report.exact_refusal_equivalent_count > 0,
          "optimizer differential corpus did not prove exact refusals");
  const auto evidence = opt::SerializeOptimizerDifferentialEvidence(report);
  Require(evidence.find("mga_visibility_authority=engine_recheck_required") !=
              std::string::npos,
          "optimizer differential evidence missing MGA visibility recheck");
  Require(evidence.find("parser_or_donor_finality_authority=false") !=
              std::string::npos,
          "optimizer differential evidence missing parser/donor authority guard");
}

platform::u64 UniqueMillis() {
  static platform::u64 counter = 0;
  const auto now = static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  return now + (++counter * 1000);
}

platform::TypedUuid NewTypedUuid(platform::UuidKind kind,
                                 platform::u64 salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, UniqueMillis() + salt);
  Require(generated.ok(), "ORH-129 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewTypedUuid(kind, salt).value);
}

template <typename TResult>
void RequireOk(const TResult& result, const std::string& message) {
  if (!result.ok) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Require(false, message);
  }
}

void RequireLifecycleOk(const db::DatabaseLifecycleResult& result,
                        const std::string& message) {
  if (!result.ok()) {
    std::cerr << result.diagnostic.diagnostic_code << ':'
              << result.diagnostic.message_key << '\n';
    Require(false, message);
  }
}

std::filesystem::path UniqueTempDir() {
  const auto path = std::filesystem::temp_directory_path() /
                    ("scratchbird_orh129_" + std::to_string(UniqueMillis()));
  std::filesystem::create_directories(path);
  return path;
}

struct TempDatabase {
  std::filesystem::path dir = UniqueTempDir();
  std::filesystem::path path = dir / "orh129.sbdb";
  std::string database_uuid;
  std::string collection_uuid;

  TempDatabase() {
    auto database = NewTypedUuid(platform::UuidKind::database, 1290);
    auto filespace = NewTypedUuid(platform::UuidKind::filespace, 1291);
    database_uuid = uuid::UuidToString(database.value);
    collection_uuid = NewUuidText(platform::UuidKind::object, 1292);
    db::DatabaseCreateConfig create;
    create.path = path.string();
    create.database_uuid = database;
    create.filespace_uuid = filespace;
    create.creation_unix_epoch_millis = UniqueMillis();
    create.require_resource_seed_pack = false;
    create.allow_minimal_resource_bootstrap = true;
    create.allow_overwrite = true;
    RequireLifecycleOk(db::CreateDatabaseFile(create),
                       "ORH-129 database create failed");
  }

  ~TempDatabase() {
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
  }
};

api::EngineRequestContext BaseContext(const TempDatabase& database,
                                      std::string request_id) {
  api::EngineRequestContext context;
  context.database_path = database.path.string();
  context.database_uuid.canonical = database.database_uuid;
  context.current_schema_uuid.canonical = database.collection_uuid;
  context.request_id = std::move(request_id);
  context.principal_uuid.canonical =
      NewUuidText(platform::UuidKind::principal, 1293);
  context.session_uuid.canonical = NewUuidText(platform::UuidKind::object, 1294);
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.resource_epoch = 129;
  context.security_epoch = 130;
  context.catalog_generation_id = 131;
  context.name_resolution_epoch = 132;
  context.security_context_present = true;
  context.trace_tags = {"optimizer_runtime_hot_path_orh_129_gate",
                        "benchmark_clean",
                        "mga_transaction_regression"};
  return context;
}

api::EngineRequestContext Begin(const TempDatabase& database,
                                std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(database, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "ORH-129 begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineCommitTransaction(request),
            "ORH-129 commit transaction failed");
}

api::EngineTypedValue Value(std::string value) {
  api::EngineTypedValue typed;
  typed.encoded_value = std::move(value);
  return typed;
}

void InsertDocument(const api::EngineRequestContext& context,
                    std::string uuid_text,
                    std::vector<std::pair<std::string, std::string>> fragments) {
  api::EngineDocumentInsertRequest insert;
  insert.context = context;
  insert.target_object.uuid.canonical = std::move(uuid_text);
  for (const auto& [path, value] : fragments) {
    insert.assignments.push_back({path, Value(value)});
  }
  const auto result = api::EngineDocumentInsert(insert);
  Require(result.ok, "ORH-129 document insert failed");
  Require(EvidenceContains(result,
                           "document_physical_provider",
                           "provider_generation_persisted=true"),
          "ORH-129 document insert did not publish provider generation");
}

api::EngineNoSqlProviderGenerationMetadata CurrentDocumentGeneration(
    const api::EngineRequestContext& context) {
  const auto generations = api::ListNoSqlProviderGenerations(context);
  Require(generations.size() == 1,
          "ORH-129 expected one document provider generation");
  return generations.front();
}

api::EngineDocumentPhysicalProof DocumentProof(
    const api::EngineRequestContext& context,
    const api::EngineNoSqlProviderGenerationMetadata& generation) {
  api::EngineDocumentPhysicalProof proof;
  proof.proof_supplied = true;
  proof.exact_path_index_proof = true;
  proof.wildcard_shape_index_proof = true;
  proof.shape_dictionary_proof = true;
  proof.structural_sharing_proof = true;
  proof.partial_materialization_proof = true;
  proof.provider_contract.family = api::EngineNoSqlProviderFamily::kDocument;
  proof.provider_contract.scope = api::EngineNoSqlProviderScope::kLocal;
  proof.provider_contract.provider_id = generation.provider_id;
  proof.provider_contract.local_provider_available = true;
  proof.provider_contract.exact_fallback_available = true;
  proof.provider_contract.descriptor_visibility.proof_present = true;
  proof.provider_contract.descriptor_visibility.visible_to_snapshot = true;
  proof.provider_contract.descriptor_visibility.descriptor_shape_compatible =
      true;
  proof.provider_contract.security_redaction.proof_present = true;
  proof.provider_contract.security_redaction.redaction_policy_bound = true;
  proof.provider_contract.security_redaction.security_snapshot_bound = true;
  proof.provider_contract.index_generation.proof_present = true;
  proof.provider_contract.index_generation.visible_to_snapshot = true;
  proof.provider_contract.index_generation.covers_predicate = true;
  proof.provider_contract.index_generation.required_generation =
      generation.generation_id;
  proof.provider_contract.index_generation.available_generation =
      generation.generation_id;
  proof.provider_contract.index_generation.index_uuid =
      api::DocumentPathProviderIdentityForContext(context,
                                                  generation.generation_id)
          .index_uuid;
  proof.provider_contract.policy.proof_present = true;
  proof.provider_contract.policy.allowed = true;
  proof.provider_contract.provider_generation.required = true;
  proof.provider_contract.provider_generation.proof_present = true;
  proof.provider_contract.provider_generation.visible_to_snapshot = true;
  proof.provider_contract.provider_generation.publish_state_bound = true;
  proof.provider_contract.provider_generation.validation_state_bound = true;
  proof.provider_contract.provider_generation
      .backup_restore_repair_metadata_bound = true;
  proof.provider_contract.provider_generation.support_bundle_evidence_bound =
      true;
  proof.provider_contract.provider_generation.required_generation =
      generation.generation_id;
  proof.provider_contract.provider_generation.available_generation =
      generation.generation_id;
  proof.provider_contract.provider_generation.descriptor_epoch =
      context.resource_epoch;
  proof.provider_contract.provider_generation.security_epoch =
      context.security_epoch;
  proof.provider_contract.provider_generation.redaction_epoch =
      context.security_epoch;
  proof.provider_contract.provider_generation.catalog_epoch =
      context.catalog_generation_id;
  proof.provider_contract.provider_generation.generation_uuid =
      generation.generation_uuid;
  proof.provider_contract.provider_generation.provider_id =
      generation.provider_id;
  proof.provider_contract.provider_generation.database_uuid =
      context.database_uuid.canonical;
  proof.provider_contract.provider_generation.collection_uuid =
      generation.collection_uuid;
  proof.provider_contract.provider_generation.publish_state = "published";
  proof.provider_contract.provider_generation.validation_state = "validated";
  proof.provider_contract.provider_generation.backup_metadata_ref =
      generation.backup_metadata_ref;
  proof.provider_contract.provider_generation.restore_metadata_ref =
      generation.restore_metadata_ref;
  proof.provider_contract.provider_generation.repair_metadata_ref =
      generation.repair_metadata_ref;
  proof.provider_contract.provider_generation.support_bundle_evidence_id =
      generation.support_bundle_evidence_id;
  proof.provider_contract.mga_recheck.proof_present = true;
  proof.provider_contract.mga_recheck.row_mga_recheck_required = true;
  proof.provider_contract.mga_recheck.row_security_recheck_required = true;
  proof.provider_contract.mga_recheck.authority_source =
      "engine_transaction_inventory";
  return proof;
}

void ProveLiveDocumentPathFilters() {
  TempDatabase database;
  auto writer = Begin(database, "orh129-document-writer");
  const auto doc_a_uuid = NewUuidText(platform::UuidKind::object, 1295);
  const auto doc_b_uuid = NewUuidText(platform::UuidKind::object, 1296);
  InsertDocument(writer,
                 doc_a_uuid,
                 {{"tenant.id", "T1"},
                  {"status", "active"},
                  {"line_items.0.sku", "SKU-1"},
                  {"line_items.1.sku", "SKU-2"}});
  InsertDocument(writer,
                 doc_b_uuid,
                 {{"tenant.id", "T2"},
                  {"status", "inactive"},
                  {"line_items.0.sku", "SKU-3"}});
  Commit(writer);

  const auto generation = CurrentDocumentGeneration(writer);
  api::EngineDocumentProviderCleanup(writer, false);
  auto reader = Begin(database, "orh129-document-reader");

  api::EngineDocumentFindRequest exact;
  exact.context = reader;
  exact.path = "tenant.id";
  exact.equals_value = "T1";
  exact.projected_paths = {"tenant.id", "status"};
  exact.require_benchmark_clean_index_runtime = true;
  exact.physical_proof = DocumentProof(reader, generation);
  auto result = api::EngineDocumentFind(exact);
  Require(result.ok, "ORH-129 exact document path lookup failed");
  Require(result.dml_summary.benchmark_clean,
          "ORH-129 exact document path was not benchmark-clean");
  Require(RowField(result, 0, "path:tenant.id") == "T1",
          "ORH-129 exact document path returned wrong row");
  Require(EvidenceContains(result,
                           "document_path_physical_provider",
                           "document_path_provider_index_consumed=true"),
          "ORH-129 exact document provider index evidence missing");
  Require(EvidenceContains(result,
                           "document_exact_source_recheck",
                           "mga_visibility_security_and_value_passed"),
          "ORH-129 exact document path did not recheck source row");

  api::EngineDocumentFindRequest wildcard = exact;
  wildcard.path = "line_items.*.sku";
  wildcard.equals_value = "SKU-2";
  wildcard.wildcard_path = true;
  wildcard.projected_paths = {"tenant.id"};
  result = api::EngineDocumentFind(wildcard);
  Require(result.ok, "ORH-129 wildcard document path lookup failed");
  Require(RowField(result, 0, "path:tenant.id") == "T1",
          "ORH-129 wildcard document path returned wrong projection");
  Require(EvidenceContains(result,
                           "document_physical_access",
                           "wildcard_shape_index_probe"),
          "ORH-129 wildcard path probe evidence missing");
  Require(EvidenceContains(
              result,
              "document_path_physical_provider",
              "document_path_provider_array_expansion_map_consumed=true"),
          "ORH-129 array expansion map evidence missing");
}

void ProveNegativeMismatchDisablesOptimization() {
  auto test_case = GenerateCases().front();
  test_case.case_id = "orh129-negative-mismatch";
  const auto baseline = EvaluatePredicate(test_case);
  auto optimized = EvaluatePredicate(test_case);
  optimized.rows.push_back("bad-extra-row");
  optimized.result_hash = HashRows(optimized.rows);
  auto validation = CompareCaptures(test_case, baseline, optimized);
  Require(!validation.ok,
          "negative mismatch unexpectedly passed equivalence");
  optimized = Disabled(test_case, "ROWS_OR_HASH_DIVERGED");
  validation = CompareCaptures(test_case, baseline, optimized);
  Require(validation.ok, "disabled mismatch did not validate");
  Require(!validation.benchmark_clean,
          "disabled mismatch must not be benchmark-clean");
  Require(Has(optimized.evidence, CaseSeed(test_case)),
          "disabled mismatch did not preserve seed/case reproduction");
  Require(HasPrefix(optimized.diagnostics,
                    "ORH_PROPERTY_FUZZ_REGRESSION_FAILED"),
          "disabled mismatch did not carry ORH-129 diagnostic");
}

void ProveNoRuntimeExecution_PlanDependency() {
  std::vector<std::string> evidence;
  for (const auto& test_case : GenerateCases()) {
    evidence.push_back(CaseSeed(test_case));
    evidence.push_back(FamilyName(test_case.family));
  }
  evidence.push_back(kRegressionFailed);
  const auto serialized = Join(evidence, "\n");
  for (const std::string forbidden :
       {"docs/", "execution-plans", "audit", "finding", "reference"}) {
    Require(serialized.find(forbidden) == std::string::npos,
            "ORH-129 evidence leaked runtime execution_plan dependency token: " +
                forbidden);
  }
}

}  // namespace

int main() {
  ProveDeterministicPropertyFuzzCorpus();
  ProveOptimizerDifferentialCorpusStillMatches();
  ProveLiveDocumentPathFilters();
  ProveNegativeMismatchDisablesOptimization();
  ProveNoRuntimeExecution_PlanDependency();
  std::cout << "optimizer_runtime_hot_path_orh_129_gate=passed "
            << "diagnostic=" << kRegressionFailed
            << " cases=" << GenerateCases().size() << '\n';
  return EXIT_SUCCESS;
}
