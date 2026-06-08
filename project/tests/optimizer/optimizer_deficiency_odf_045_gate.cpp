// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "nosql/search_api.hpp"
#include "nosql/time_series_api.hpp"
#include "nosql/vector_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().message_key << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

platform::u64 UniqueSeed() {
  static platform::u64 counter = 0;
  return NowMillis() + (++counter * 1000);
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, UniqueSeed() + salt);
  Require(generated.ok(), "ODF-045 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    std::error_code ignored;
    if (!dir.empty()) { std::filesystem::remove_all(dir, ignored); }
  }
};

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && item.evidence_id == id) { return true; }
  }
  return false;
}

bool EvidenceContains(const std::vector<api::EngineEvidenceReference>& evidence,
                      std::string_view kind,
                      std::string_view token) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind &&
        item.evidence_id.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool AnyEvidenceContains(const std::vector<api::EngineEvidenceReference>& evidence,
                         std::string_view token) {
  for (const auto& item : evidence) {
    if (item.evidence_kind.find(token) != std::string::npos ||
        item.evidence_id.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::size_t EvidenceIndex(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind,
                          std::string_view id) {
  for (std::size_t index = 0; index < evidence.size(); ++index) {
    if (evidence[index].evidence_kind == kind &&
        evidence[index].evidence_id == id) {
      return index;
    }
  }
  return evidence.size();
}

void AssertNoRuntimeDocLeaks(const api::EngineApiResult& result) {
  const std::vector<std::string_view> forbidden = {
      "docs" "/execution-plans", "execution_plan", "findings", "contracts", "references"};
  for (const auto& evidence : result.evidence) {
    for (const auto token : forbidden) {
      Require(evidence.evidence_kind.find(token) == std::string::npos &&
                  evidence.evidence_id.find(token) == std::string::npos,
              "ODF-045 runtime evidence leaked documentation token");
    }
  }
}

Fixture MakeFixture(std::string name, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_odf045_" + name + "_" +
                 std::to_string(UniqueSeed()));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf045.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = UniqueSeed();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ODF-045 database create failed");
  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  return fixture;
}

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical =
      NewUuidText(platform::UuidKind::principal, fixture.salt + 100);
  context.session_uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 101);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 45;
  context.security_epoch = 46;
  context.resource_epoch = 47;
  context.name_resolution_epoch = 48;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "ODF-045 begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "ODF-045 rollback failed");
}

template <typename TRequest>
TRequest HeavyGenerationRequest(const api::EngineRequestContext& context,
                                const std::string& target_uuid,
                                const std::string& target_kind,
                                const std::string& generation_uuid,
                                const std::string& proof_suffix,
                                bool include_validation_proof = true) {
  TRequest request;
  request.context = context;
  request.target_object.uuid.canonical = target_uuid;
  request.target_object.object_kind = target_kind;
  request.option_envelopes.push_back("heavy_generation.generation_uuid=" +
                                     generation_uuid);
  request.option_envelopes.push_back("heavy_generation.source_row_count=3");
  request.option_envelopes.push_back("heavy_generation.source_payload_count=3");
  if (include_validation_proof) {
    request.option_envelopes.push_back("heavy_generation.validation_proof=proof-" +
                                       proof_suffix);
  }
  request.option_envelopes.push_back("heavy_generation.validation_succeeded=true");
  request.option_envelopes.push_back(
      "heavy_generation.immutable_payload_complete=true");
  request.option_envelopes.push_back(
      "heavy_generation.source_counts_verified=true");
  request.option_envelopes.push_back("heavy_generation.checksum_verified=true");
  request.option_envelopes.push_back(
      "heavy_generation.mga_authority=engine_mga_transaction_inventory");
  request.option_envelopes.push_back(
      "heavy_generation.mga_inventory=local_transaction_inventory_" +
      std::to_string(context.local_transaction_id));
  request.option_envelopes.push_back(
      "heavy_generation.publication_fence=publication_fence_" +
      generation_uuid);
  request.option_envelopes.push_back(
      "heavy_generation.engine_owned_mga_publication_fence=true");
  return request;
}

void AssertPublishedGeneration(const api::EngineApiResult& result,
                               std::string_view operation_evidence_kind,
                               std::string_view surface,
                               std::string_view family,
                               std::string_view profile) {
  RequireOk(result, "ODF-045 heavy-family operation failed");
  Require(HasEvidence(result.evidence,
                      "heavy_immutable_generation_validation_state",
                      "validated"),
          "ODF-045 validation-state evidence missing");
  Require(HasEvidence(result.evidence,
                      "heavy_immutable_generation_publication_state",
                      "published"),
          "ODF-045 publication-state evidence missing");
  Require(HasEvidence(result.evidence,
                      "heavy_immutable_generation_family",
                      family),
          "ODF-045 family evidence mismatch");
  Require(HasEvidence(result.evidence,
                      "heavy_immutable_generation_profile",
                      profile),
          "ODF-045 profile evidence mismatch");
  Require(EvidenceContains(result.evidence,
                           "heavy_immutable_generation_id",
                           ""),
          "ODF-045 immutable generation id evidence missing");
  Require(HasEvidence(result.evidence,
                      "heavy_immutable_generation_mga_authority",
                      "engine_mga_transaction_inventory"),
          "ODF-045 MGA authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "mga_finality_authority",
                      "engine_transaction_inventory"),
          "ODF-045 MGA finality authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "heavy_immutable_generation_finality_authority",
                      "false"),
          "ODF-045 generation finality authority guard missing");
  Require(HasEvidence(result.evidence,
                      "heavy_immutable_generation_descriptor_only",
                      "true"),
          "ODF-045 descriptor-only evidence missing");
  Require(EvidenceContains(result.evidence,
                           "heavy_immutable_generation_publication_fence",
                           "publication_fence_"),
          "ODF-045 publication fence evidence missing");
  Require(HasEvidence(result.evidence,
                      operation_evidence_kind,
                      "validated_immutable_generation_published"),
          "ODF-045 operation publish evidence missing");
  Require(HasEvidence(result.evidence, "nosql_surface", surface),
          "ODF-045 NoSQL surface evidence missing");
  Require(HasEvidence(result.evidence,
                      "cluster_provider_dispatch",
                      "false"),
          "ODF-045 cluster provider dispatch evidence drifted");
  Require(EvidenceIndex(result.evidence,
                        "heavy_immutable_generation_validation_state",
                        "validated") <
              EvidenceIndex(result.evidence,
                            "heavy_immutable_generation_publication_state",
                            "published"),
          "ODF-045 publication evidence preceded validation evidence");
  AssertNoRuntimeDocLeaks(result);
}

void TextVectorAndColumnarPublishValidatedGenerations() {
  auto fixture = MakeFixture("publish", 45000);
  auto context = Begin(fixture, "odf045-publish");
  const auto text_collection =
      NewUuidText(platform::UuidKind::object, fixture.salt + 10);
  const auto vector_collection =
      NewUuidText(platform::UuidKind::object, fixture.salt + 11);
  const auto time_series_collection =
      NewUuidText(platform::UuidKind::object, fixture.salt + 12);

  const auto text = api::EngineSearchQuery(
      HeavyGenerationRequest<api::EngineSearchQueryRequest>(
          context,
          text_collection,
          "search_collection",
          NewUuidText(platform::UuidKind::object, fixture.salt + 20),
          "text"));
  AssertPublishedGeneration(text,
                            "search_query",
                            "search",
                            "text_search",
                            "text_search_immutable_segment_v1");

  const auto vector = api::EngineVectorSearch(
      HeavyGenerationRequest<api::EngineVectorSearchRequest>(
          context,
          vector_collection,
          "vector_collection",
          NewUuidText(platform::UuidKind::object, fixture.salt + 21),
          "vector"));
  AssertPublishedGeneration(vector,
                            "vector_search",
                            "vector",
                            "vector",
                            "vector_immutable_ann_generation_v1");

  const auto columnar = api::EngineTimeSeriesAppend(
      HeavyGenerationRequest<api::EngineTimeSeriesAppendRequest>(
          context,
          time_series_collection,
          "time_series",
          NewUuidText(platform::UuidKind::object, fixture.salt + 22),
          "columnar"));
  AssertPublishedGeneration(columnar,
                            "time_series_append",
                            "time_series",
                            "columnar_summary",
                            "time_series_columnar_summary_generation_v1");
  Rollback(context);
}

void MissingValidationProofFailsBeforePublication() {
  auto fixture = MakeFixture("missing-proof", 45500);
  auto context = Begin(fixture, "odf045-missing-proof");
  const auto collection =
      NewUuidText(platform::UuidKind::object, fixture.salt + 30);
  const auto generation =
      NewUuidText(platform::UuidKind::object, fixture.salt + 31);
  const auto refused = api::EngineSearchQuery(
      HeavyGenerationRequest<api::EngineSearchQueryRequest>(
          context,
          collection,
          "search_collection",
          generation,
          "missing",
          false));
  Require(!refused.ok, "ODF-045 missing validation proof was accepted");
  Require(!refused.diagnostics.empty(),
          "ODF-045 missing validation proof lacked diagnostic");
  Require(refused.diagnostics.front().code ==
              "INDEX.HEAVY_IMMUTABLE_GENERATION.VALIDATION_PROOF_MISSING",
          "ODF-045 missing validation proof diagnostic drifted");
  Require(HasEvidence(refused.evidence,
                      "heavy_immutable_generation_fail_closed",
                      "true"),
          "ODF-045 missing validation proof did not fail closed");
  Require(!HasEvidence(refused.evidence,
                       "heavy_immutable_generation_publication_state",
                       "published"),
          "ODF-045 refusal emitted publication success evidence");
  Require(!HasEvidence(refused.evidence,
                       "search_query",
                       "validated_immutable_generation_published"),
          "ODF-045 refusal emitted operation success evidence");
  Require(HasEvidence(refused.evidence,
                      "heavy_immutable_generation_mga_authority",
                      "engine_mga_transaction_inventory"),
          "ODF-045 refusal lost MGA authority evidence");
  Require(HasEvidence(refused.evidence,
                      "mga_finality_authority",
                      "engine_transaction_inventory"),
          "ODF-045 refusal lost MGA finality authority evidence");
  AssertNoRuntimeDocLeaks(refused);
  Rollback(context);
}

void ExistingNoSqlRoutesRemainBackwardCompatible() {
  auto fixture = MakeFixture("fallback", 45600);
  auto context = Begin(fixture, "odf045-fallback");

  api::EngineSearchQueryRequest search;
  search.context = context;
  search.target_object.uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 40);
  search.target_object.object_kind = "search_collection";
  const auto search_result = api::EngineSearchQuery(search);
  RequireOk(search_result, "ODF-045 search fallback failed");
  Require(HasEvidence(search_result.evidence,
                      "search_query",
                      "full_text_descriptor_query"),
          "ODF-045 search fallback evidence drifted");
  Require(HasEvidence(search_result.evidence,
                      "nosql_behavior",
                      "specialized_descriptor_fallback"),
          "ODF-045 search behavior fallback evidence missing");
  Require(!AnyEvidenceContains(search_result.evidence,
                               "heavy_immutable_generation"),
          "ODF-045 search fallback unexpectedly published generation");

  api::EngineVectorSearchRequest vector;
  vector.context = context;
  vector.target_object.uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 41);
  vector.target_object.object_kind = "vector_collection";
  const auto vector_result = api::EngineVectorSearch(vector);
  RequireOk(vector_result, "ODF-045 vector fallback failed");
  Require(HasEvidence(vector_result.evidence,
                      "vector_search",
                      "exact_fallback_available"),
          "ODF-045 vector fallback evidence drifted");
  Require(HasEvidence(vector_result.evidence,
                      "nosql_behavior",
                      "exact_scan_until_vector_index_available"),
          "ODF-045 vector behavior fallback evidence missing");
  Require(!AnyEvidenceContains(vector_result.evidence,
                               "heavy_immutable_generation"),
          "ODF-045 vector fallback unexpectedly published generation");

  api::EngineTimeSeriesAppendRequest time_series;
  time_series.context = context;
  time_series.target_object.uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 42);
  time_series.target_object.object_kind = "time_series";
  const auto time_series_result = api::EngineTimeSeriesAppend(time_series);
  RequireOk(time_series_result, "ODF-045 time-series fallback failed");
  Require(HasEvidence(time_series_result.evidence,
                      "nosql_behavior",
                      "persisted_time_series_append"),
          "ODF-045 time-series fallback evidence drifted");
  Require(!AnyEvidenceContains(time_series_result.evidence,
                               "heavy_immutable_generation"),
          "ODF-045 time-series fallback unexpectedly published generation");

  Rollback(context);
}

}  // namespace

int main() {
  TextVectorAndColumnarPublishValidatedGenerations();
  MissingValidationProofFailsBeforePublication();
  ExistingNoSqlRoutesRemainBackwardCompatible();
  return EXIT_SUCCESS;
}
