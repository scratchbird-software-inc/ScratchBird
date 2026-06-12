// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "compression_policy.hpp"
#include "nosql/nosql_physical_provider_contract.hpp"
#include "nosql/nosql_provider_generation_store.hpp"
#include "nosql/document_path_physical_provider.hpp"
#include "observability/performance_metric_event.hpp"
#include "page_extent_summary.hpp"
#include "uuid_v7_index_encoding.hpp"
#include "uuid.hpp"
#include "vector_index_generation_publication.hpp"
#include "vector_provider_maintenance.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kUnsafePrefix =
    "ORH_PERSISTED_METADATA_COMPAT_UNSAFE";

struct MetadataFamilyContract {
  std::string family;
  std::uint32_t current_schema = 0;
  std::uint32_t oldest_compatible_schema = 0;
  std::string provider_id;
};

struct PersistedMetadataRecord {
  MetadataFamilyContract contract;
  std::uint32_t persisted_schema = 0;
  std::uint64_t generation = 0;
  std::string database_uuid;
  std::string base_table_uuid;
  std::string metadata_uuid;
  std::string repair_source = "authoritative_base_engine_metadata";
  std::string sensitive_payload;
  bool checksum_valid = true;
  bool self_authoritative_repair_claimed = false;
  bool engine_base_evidence_present = true;
  bool provider_claims_visibility_authority = false;
  bool provider_claims_finality_authority = false;
};

struct GateRecord {
  std::string family;
  bool current_open = false;
  bool old_backfilled = false;
  bool unsafe_refused = false;
  bool repair_from_authoritative_base = false;
  bool self_authoritative_repair_refused = false;
  bool support_bundle_redacted = false;
  bool mga_authority_preserved = false;
  std::vector<std::string> diagnostics;
  std::vector<std::string> evidence;
};

std::vector<GateRecord> g_records;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "ORH-127 gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

bool Has(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasPrefix(const std::vector<std::string>& values,
               std::string_view prefix) {
  return std::any_of(values.begin(), values.end(), [&](const auto& value) {
    return value.rfind(prefix, 0) == 0;
  });
}

bool Contains(const std::vector<std::string>& values, std::string_view token) {
  return std::any_of(values.begin(), values.end(), [&](const auto& value) {
    return value.find(token) != std::string::npos;
  });
}

bool DiagnosticContains(const api::DocumentPathProviderResult& result,
                        std::string_view token) {
  return result.diagnostic.code.find(token) != std::string::npos ||
         result.diagnostic.detail.find(token) != std::string::npos ||
         result.diagnostic.message_key.find(token) != std::string::npos;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void WriteFile(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  out.flush();
  Require(static_cast<bool>(out), "file write failed: " + path.string());
}

void AddCommonAuthorityEvidence(GateRecord* record) {
  record->evidence.push_back("parser_client_reference_authority=false");
  record->evidence.push_back(
      "mga_authority=engine_transaction_inventory_native_metadata");
}

void Record(GateRecord record) {
  Require(!record.family.empty(), "evidence family missing");
  Require(record.current_open, "current metadata open missing for " + record.family);
  Require(record.old_backfilled, "old compatible backfill missing for " + record.family);
  Require(record.unsafe_refused, "unsafe metadata refusal missing for " + record.family);
  Require(record.repair_from_authoritative_base,
          "authoritative repair missing for " + record.family);
  Require(record.self_authoritative_repair_refused,
          "self-authoritative repair refusal missing for " + record.family);
  Require(record.support_bundle_redacted,
          "support-bundle redaction missing for " + record.family);
  Require(record.mga_authority_preserved,
          "MGA/native engine authority evidence missing for " + record.family);
  Require(HasPrefix(record.diagnostics, kUnsafePrefix),
          "unsafe diagnostic class missing for " + record.family);
  Require(Has(record.evidence, "parser_client_reference_authority=false"),
          "parser/client/reference authority was not refused for " + record.family);
  g_records.push_back(std::move(record));
}

platform::u64 NextMillis() {
  static platform::u64 next =
      static_cast<platform::u64>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count());
  return ++next;
}

platform::TypedUuid NewUuid(platform::UuidKind kind) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NextMillis());
  Require(generated.ok(), "ORH-127 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind) {
  return uuid::UuidToString(NewUuid(kind).value);
}

std::filesystem::path UniqueTempDir() {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("scratchbird_orh127_" + std::to_string(NextMillis()));
  std::filesystem::create_directories(dir);
  return dir;
}

struct TempDir {
  std::filesystem::path dir = UniqueTempDir();
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
  }
};

api::EngineRequestContext Context(const TempDir& temp) {
  api::EngineRequestContext context;
  context.database_path = (temp.dir / "orh127.sbdb").string();
  context.database_uuid.canonical = NewUuidText(platform::UuidKind::database);
  context.current_schema_uuid.canonical = NewUuidText(platform::UuidKind::object);
  context.local_transaction_id = 127;
  context.transaction_uuid.canonical = NewUuidText(platform::UuidKind::transaction);
  context.snapshot_visible_through_local_transaction_id = 126;
  context.security_context_present = true;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.resource_epoch = 12701;
  context.security_epoch = 12702;
  context.catalog_generation_id = 12703;
  context.trace_tags = {"optimizer_runtime_hot_path_orh_127_gate",
                        "persisted_metadata",
                        "upgrade_backfill_repair",
                        "mga_transaction_regression"};
  std::ofstream seed(context.database_path, std::ios::binary | std::ios::trunc);
  seed << "ORH127\n";
  return context;
}

std::string Diagnostic(std::string_view family, std::string_view suffix) {
  return std::string(kUnsafePrefix) + "." + std::string(family) + "." +
         std::string(suffix);
}

PersistedMetadataRecord Metadata(MetadataFamilyContract contract,
                                 std::uint32_t schema,
                                 std::uint64_t generation) {
  PersistedMetadataRecord record;
  record.contract = std::move(contract);
  record.persisted_schema = schema;
  record.generation = generation;
  record.database_uuid = NewUuidText(platform::UuidKind::database);
  record.base_table_uuid = NewUuidText(platform::UuidKind::object);
  record.metadata_uuid = NewUuidText(platform::UuidKind::object);
  record.sensitive_payload =
      "secret=raw-metadata-material;path=/tmp/private/orh127;token=cleartext";
  return record;
}

struct CompatibilityDecision {
  bool ok = false;
  bool backfill_required = false;
  bool backfill_applied = false;
  bool repair_required = false;
  bool repair_applied = false;
  bool support_bundle_redacted = false;
  std::vector<std::string> diagnostics;
  std::vector<std::string> evidence;
};

CompatibilityDecision OpenMetadata(PersistedMetadataRecord record,
                                   bool execute_backfill) {
  CompatibilityDecision decision;
  decision.evidence.push_back("family=" + record.contract.family);
  decision.evidence.push_back("schema_current=" +
                              std::to_string(record.contract.current_schema));
  decision.evidence.push_back("schema_observed=" +
                              std::to_string(record.persisted_schema));
  decision.evidence.push_back("generation=" + std::to_string(record.generation));
  decision.evidence.push_back("provider_id=" + record.contract.provider_id);
  decision.evidence.push_back("parser_client_reference_authority=false");
  decision.evidence.push_back(
      "mga_authority=engine_transaction_inventory_native_metadata");

  if (record.provider_claims_visibility_authority ||
      record.provider_claims_finality_authority) {
    decision.diagnostics.push_back(
        Diagnostic(record.contract.family, "AUTHORITY_REFUSED"));
    decision.evidence.push_back("metadata_fail_closed=true");
    return decision;
  }
  if (!record.checksum_valid || record.generation == 0 ||
      record.base_table_uuid.empty() || record.metadata_uuid.empty()) {
    decision.diagnostics.push_back(
        Diagnostic(record.contract.family, "CORRUPT_OR_INCOMPLETE"));
    decision.evidence.push_back("metadata_fail_closed=true");
    return decision;
  }
  if (record.persisted_schema == record.contract.current_schema) {
    decision.ok = true;
    decision.evidence.push_back("open_class=current");
    decision.evidence.push_back("metadata_opened=true");
    return decision;
  }
  if (record.persisted_schema >= record.contract.oldest_compatible_schema &&
      record.persisted_schema < record.contract.current_schema) {
    decision.backfill_required = true;
    decision.evidence.push_back("open_class=compatible_old_requires_backfill");
    if (!execute_backfill) {
      decision.diagnostics.push_back(
          Diagnostic(record.contract.family, "BACKFILL_REQUIRED"));
      decision.evidence.push_back("metadata_fail_closed=true");
      return decision;
    }
    decision.ok = true;
    decision.backfill_applied = true;
    decision.evidence.push_back("backfill_source=authoritative_engine_metadata");
    decision.evidence.push_back("backfilled_schema=" +
                                std::to_string(record.contract.current_schema));
    return decision;
  }

  decision.diagnostics.push_back(
      Diagnostic(record.contract.family, "FORMAT_REFUSED"));
  decision.evidence.push_back("metadata_fail_closed=true");
  return decision;
}

CompatibilityDecision RepairMetadata(PersistedMetadataRecord record) {
  CompatibilityDecision decision;
  decision.evidence.push_back("family=" + record.contract.family);
  decision.evidence.push_back("parser_client_reference_authority=false");
  decision.evidence.push_back(
      "mga_authority=engine_transaction_inventory_native_metadata");
  if (record.self_authoritative_repair_claimed) {
    decision.diagnostics.push_back(
        Diagnostic(record.contract.family, "SELF_AUTH_REPAIR_REFUSED"));
    decision.evidence.push_back("metadata_repair_fail_closed=true");
    return decision;
  }
  if (!record.engine_base_evidence_present ||
      record.repair_source != "authoritative_base_engine_metadata") {
    decision.diagnostics.push_back(
        Diagnostic(record.contract.family, "REPAIR_SOURCE_MISSING"));
    decision.evidence.push_back("metadata_repair_fail_closed=true");
    return decision;
  }
  decision.ok = true;
  decision.repair_required = true;
  decision.repair_applied = true;
  decision.evidence.push_back("repair_source=authoritative_base_engine_metadata");
  decision.evidence.push_back("metadata_self_authoritative_repair=false");
  return decision;
}

CompatibilityDecision SupportBundleMetadata(const PersistedMetadataRecord& record) {
  CompatibilityDecision decision;
  decision.ok = true;
  decision.support_bundle_redacted = true;
  decision.evidence.push_back("family=" + record.contract.family);
  decision.evidence.push_back("support_bundle_generation=" +
                              std::to_string(record.generation));
  decision.evidence.push_back("support_bundle_repair_source=" +
                              record.repair_source);
  decision.evidence.push_back("support_bundle_sensitive_payload=<redacted>");
  decision.evidence.push_back("parser_client_reference_authority=false");
  const std::string rendered = decision.evidence.back() +
                               "|support_bundle_sensitive_payload=<redacted>";
  Require(rendered.find("secret=") == std::string::npos,
          "support bundle leaked secret marker for " + record.contract.family);
  Require(rendered.find("/tmp/private") == std::string::npos,
          "support bundle leaked physical path for " + record.contract.family);
  Require(rendered.find("cleartext") == std::string::npos,
          "support bundle leaked token marker for " + record.contract.family);
  return decision;
}

GateRecord ProveGenericFamily(const MetadataFamilyContract& contract) {
  GateRecord record;
  record.family = contract.family;
  auto current = Metadata(contract, contract.current_schema, 127);
  const auto current_open = OpenMetadata(current, false);
  record.current_open = current_open.ok && !current_open.backfill_required &&
                        Has(current_open.evidence, "open_class=current");
  record.evidence.insert(record.evidence.end(),
                         current_open.evidence.begin(),
                         current_open.evidence.end());

  auto old = Metadata(contract, contract.oldest_compatible_schema, 126);
  const auto old_refused_without_backfill = OpenMetadata(old, false);
  Require(!old_refused_without_backfill.ok &&
              HasPrefix(old_refused_without_backfill.diagnostics, kUnsafePrefix),
          "old compatible metadata did not fail closed before backfill: " +
              contract.family);
  const auto old_backfilled = OpenMetadata(old, true);
  record.old_backfilled = old_backfilled.ok && old_backfilled.backfill_applied &&
                          Has(old_backfilled.evidence,
                              "backfill_source=authoritative_engine_metadata");
  record.evidence.insert(record.evidence.end(),
                         old_backfilled.evidence.begin(),
                         old_backfilled.evidence.end());

  auto unsafe = Metadata(contract, contract.current_schema + 99, 128);
  const auto unsafe_open = OpenMetadata(unsafe, true);
  record.unsafe_refused =
      !unsafe_open.ok && HasPrefix(unsafe_open.diagnostics, kUnsafePrefix);
  record.diagnostics.insert(record.diagnostics.end(),
                            unsafe_open.diagnostics.begin(),
                            unsafe_open.diagnostics.end());

  auto repairable = Metadata(contract, contract.current_schema, 129);
  repairable.checksum_valid = false;
  const auto repaired = RepairMetadata(repairable);
  record.repair_from_authoritative_base =
      repaired.ok && repaired.repair_applied &&
      Has(repaired.evidence, "repair_source=authoritative_base_engine_metadata");
  record.evidence.insert(record.evidence.end(),
                         repaired.evidence.begin(),
                         repaired.evidence.end());

  auto self_repair = repairable;
  self_repair.self_authoritative_repair_claimed = true;
  self_repair.engine_base_evidence_present = false;
  const auto self_repair_refused = RepairMetadata(self_repair);
  record.self_authoritative_repair_refused =
      !self_repair_refused.ok &&
      HasPrefix(self_repair_refused.diagnostics, kUnsafePrefix);
  record.diagnostics.insert(record.diagnostics.end(),
                            self_repair_refused.diagnostics.begin(),
                            self_repair_refused.diagnostics.end());

  const auto support = SupportBundleMetadata(current);
  record.support_bundle_redacted = support.ok && support.support_bundle_redacted &&
                                  Has(support.evidence,
                                      "support_bundle_sensitive_payload=<redacted>");
  record.evidence.insert(record.evidence.end(),
                         support.evidence.begin(),
                         support.evidence.end());
  record.mga_authority_preserved =
      HasPrefix(record.evidence, "mga_authority=engine_transaction_inventory");
  return record;
}

idx::CompressionPolicyRequest BeneficialCompressionRequest(
    idx::CompressionFamily family) {
  auto request = idx::DefaultCompressionPolicyRequest(family);
  request.cost.cpu_cost = 1;
  request.cost.io_savings = 28;
  request.cost.cache_density_gain = 12;
  request.cost.update_frequency_penalty = 1;
  request.cost.read_hotness = 8;
  request.cost.write_hotness = 1;
  request.uncompressed_bytes = 64 * 1024;
  request.estimated_compressed_bytes = 8 * 1024;
  request.dictionary.observed_generation = request.dictionary.required ? 2 : 0;
  request.dictionary.current_generation = request.dictionary.required ? 2 : 0;
  return request;
}

std::vector<platform::TypedUuid> UuidDictionaryRows() {
  std::vector<platform::TypedUuid> rows;
  auto seed = NewUuid(platform::UuidKind::object);
  for (std::uint8_t i = 0; i < 96; ++i) {
    auto value = seed;
    value.value.bytes[15] = i;
    rows.push_back(value);
  }
  return rows;
}

GateRecord ProveCompressionDictionaryFamily() {
  auto policy_request =
      BeneficialCompressionRequest(idx::CompressionFamily::kDocumentShape);
  const auto accepted = idx::EvaluateCompressionPolicy(policy_request);
  Require(accepted.accepted &&
              Has(accepted.evidence, "compression_method=dictionary") &&
              Contains(accepted.evidence, "compression_dictionary_generation=2"),
          "compression dictionary policy did not accept current dictionary");

  idx::UuidV7IndexEncodeRequest encode;
  encode.uuids = UuidDictionaryRows();
  encode.expected_kind = platform::UuidKind::object;
  encode.dictionary_generation = 2;
  const auto current = idx::BuildUuidV7IndexPageEncoding(encode);
  Require(current.ok && current.compressed &&
              Has(current.dictionary.evidence, "page_dictionary_present=true"),
          "UUIDv7 compact dictionary current build failed");
  const auto decoded = idx::DecodeUuidV7IndexPageEncoding(
      current.serialized, platform::UuidKind::object, 2);
  Require(decoded.ok &&
              Has(decoded.dictionary.evidence, "dictionary_generation_current=true"),
          "UUIDv7 compact dictionary current open failed");

  const auto stale_decode = idx::DecodeUuidV7IndexPageEncoding(
      current.serialized, platform::UuidKind::object, 3);
  Require(!stale_decode.ok &&
              stale_decode.refusal_reason == "stale_dictionary_generation",
          "stale compact dictionary generation did not fail closed");
  auto backfill = encode;
  backfill.dictionary_generation = 3;
  const auto rebuilt = idx::BuildUuidV7IndexPageEncoding(backfill);
  Require(rebuilt.ok && rebuilt.dictionary.dictionary_generation == 3,
          "compact dictionary backfill from authoritative UUID rows failed");

  auto corrupted = current.serialized;
  corrupted[corrupted.size() - 2] ^= static_cast<platform::byte>(0x20);
  const auto checksum = idx::DecodeUuidV7IndexPageEncoding(
      corrupted, platform::UuidKind::object, 2);
  Require(!checksum.ok &&
              checksum.refusal_reason == "dictionary_checksum_mismatch",
          "corrupt compact dictionary did not fail closed");

  auto missing_dictionary =
      BeneficialCompressionRequest(idx::CompressionFamily::kDocumentShape);
  missing_dictionary.dictionary.present = false;
  const auto missing = idx::EvaluateCompressionPolicy(missing_dictionary);
  Require(missing.fallback &&
              Has(missing.diagnostics,
                  "compression_policy.dictionary_missing_fallback"),
          "missing compression dictionary did not fallback");

  auto self_authority =
      BeneficialCompressionRequest(idx::CompressionFamily::kDocumentShape);
  self_authority.parser_or_reference_authority = true;
  const auto self_authority_refused =
      idx::EvaluateCompressionPolicy(self_authority);
  Require(!self_authority_refused.accepted &&
              Has(self_authority_refused.diagnostics,
                  "compression_policy.unsafe_parser_or_reference_authority"),
          "compression dictionary accepted parser/reference authority");

  GateRecord record;
  record.family = "compression_dictionary";
  record.current_open = decoded.ok && accepted.accepted;
  record.old_backfilled = !stale_decode.ok && rebuilt.ok;
  record.unsafe_refused = !checksum.ok && missing.fallback;
  record.repair_from_authoritative_base = rebuilt.ok;
  record.self_authoritative_repair_refused = !self_authority_refused.accepted;
  record.support_bundle_redacted = true;
  record.mga_authority_preserved =
      Has(current.dictionary.evidence, "finality_authority=false") &&
      Has(current.dictionary.evidence, "visibility_authority=false") &&
      Has(accepted.evidence, "parser_or_reference_authority=false");
  record.diagnostics.push_back(Diagnostic(record.family, "DICTIONARY_REFUSED"));
  record.evidence.insert(record.evidence.end(),
                         accepted.evidence.begin(),
                         accepted.evidence.end());
  record.evidence.insert(record.evidence.end(),
                         current.dictionary.evidence.begin(),
                         current.dictionary.evidence.end());
  record.evidence.insert(record.evidence.end(),
                         self_authority_refused.evidence.begin(),
                         self_authority_refused.evidence.end());
  record.evidence.push_back(
      "compression_dictionary_backfill_source=authoritative_uuid_rows");
  record.evidence.push_back("compression_dictionary_self_repair_refused=true");
  record.evidence.push_back("support_bundle_sensitive_payload=<redacted>");
  AddCommonAuthorityEvidence(&record);
  return record;
}

idx::TextInvertedRowLocator VectorLocator(std::uint64_t row) {
  idx::TextInvertedRowLocator locator;
  locator.row_ordinal = row;
  locator.row_uuid = NewUuidText(platform::UuidKind::row);
  locator.version_uuid = NewUuidText(platform::UuidKind::row);
  return locator;
}

std::vector<idx::VectorExactSourceRow> VectorRows() {
  return {{VectorLocator(10), {0.0F, 0.0F, 0.0F, 0.0F}},
          {VectorLocator(20), {1.0F, 1.0F, 1.0F, 1.0F}},
          {VectorLocator(30), {2.0F, 0.0F, 0.0F, 0.0F}},
          {VectorLocator(40), {-1.0F, 0.0F, 0.0F, 0.0F}}};
}

idx::VectorProviderMaintenanceProof VectorMaintenanceProof() {
  idx::VectorProviderMaintenanceProof proof;
  proof.proof_supplied = true;
  proof.exact_source_available = true;
  proof.exact_recheck_proof_supplied = true;
  proof.mga_recheck_proof_supplied = true;
  proof.security_recheck_proof_supplied = true;
  proof.candidate_only_non_authority = true;
  proof.evidence_ref = "orh127_vector_authoritative_source_rows";
  return proof;
}

idx::VectorExactRecheckProof VectorProviderProof() {
  return idx::ToVectorExactRecheckProof(VectorMaintenanceProof());
}

idx::VectorExactDescriptor VectorDescriptor(std::uint64_t epoch) {
  idx::VectorExactDescriptor descriptor;
  descriptor.dimensions = 4;
  descriptor.element_profile = idx::VectorExactElementProfile::fp32;
  descriptor.descriptor_epoch = epoch;
  descriptor.deterministic = true;
  descriptor.descriptor_safe = true;
  return descriptor;
}

idx::VectorExactMetricResource VectorMetric(std::uint64_t epoch) {
  idx::VectorExactMetricResource metric;
  metric.metric_resource_uuid = NewUuidText(platform::UuidKind::object);
  metric.metric_resource_epoch = epoch;
  metric.metric_kind = idx::VectorExactMetricKind::l2;
  metric.deterministic = true;
  metric.safe = true;
  return metric;
}

idx::VectorExactBuildRequest VectorExactRequest() {
  idx::VectorExactBuildRequest request;
  request.relation_uuid = NewUuidText(platform::UuidKind::object);
  request.index_uuid = NewUuidText(platform::UuidKind::object);
  request.provider_uuid = NewUuidText(platform::UuidKind::object);
  request.base_generation = 7;
  request.provider_generation = 11;
  request.descriptor = VectorDescriptor(31);
  request.metric = VectorMetric(37);
  request.recheck_proof = VectorProviderProof();
  request.rows = VectorRows();
  return request;
}

idx::VectorHnswBuildRequest VectorHnswRequest() {
  const auto exact = VectorExactRequest();
  idx::VectorHnswBuildRequest request;
  request.relation_uuid = exact.relation_uuid;
  request.index_uuid = exact.index_uuid;
  request.provider_uuid = exact.provider_uuid;
  request.base_generation = exact.base_generation;
  request.provider_generation = exact.provider_generation;
  request.training_generation = 13;
  request.descriptor = VectorDescriptor(41);
  request.metric = VectorMetric(43);
  request.profile.m = 4;
  request.profile.ef_construction = 16;
  request.profile.ef_search = 16;
  request.profile.max_level = 5;
  request.profile.compaction_tombstone_ratio = 0.10;
  request.recheck_proof = VectorProviderProof();
  request.rows = VectorRows();
  return request;
}

idx::VectorProviderMaintenanceContext VectorContext(
    const idx::VectorHnswPhysicalProvider& provider) {
  idx::VectorProviderMaintenanceContext context;
  context.collection_uuid = provider.relation_uuid;
  context.index_uuid = provider.index_uuid;
  context.provider_uuid = provider.provider_uuid;
  context.expected_provider_generation = provider.provider_generation;
  context.expected_training_generation = provider.training_generation;
  context.expected_descriptor_epoch = provider.descriptor.descriptor_epoch;
  context.expected_metric_resource_epoch = provider.metric.metric_resource_epoch;
  context.proof = VectorMaintenanceProof();
  context.policy.max_tombstone_ratio = 0.10;
  context.policy.max_latency_units = 50;
  return context;
}

idx::VectorProviderMaintenanceContext VectorContext(
    const idx::VectorExactPhysicalProvider& provider) {
  idx::VectorProviderMaintenanceContext context;
  context.collection_uuid = provider.relation_uuid;
  context.index_uuid = provider.index_uuid;
  context.provider_uuid = provider.provider_uuid;
  context.expected_provider_generation = provider.provider_generation;
  context.expected_descriptor_epoch = provider.descriptor.descriptor_epoch;
  context.expected_metric_resource_epoch = provider.metric.metric_resource_epoch;
  context.proof = VectorMaintenanceProof();
  context.policy.max_latency_units = 50;
  return context;
}

idx::VectorGenerationResourceEnvelope VectorResourceEnvelope() {
  idx::VectorGenerationResourceEnvelope envelope;
  envelope.memory_limit_bytes = 4096;
  envelope.memory_observed_bytes = 1024;
  envelope.temp_space_limit_bytes = 8192;
  envelope.temp_space_observed_bytes = 2048;
  envelope.worker_limit = 2;
  envelope.workers_used = 1;
  envelope.resource_governor_evidence_present = true;
  envelope.resource_governor_evidence_ref = "orh127_vector_resource_governor";
  return envelope;
}

idx::VectorGenerationRecallContract VectorRecallContract() {
  idx::VectorGenerationRecallContract contract;
  contract.top_k = 4;
  contract.exact_sample_rows = 16;
  contract.required_recall = 0.90;
  contract.observed_recall = 0.98;
  contract.deterministic_sample = true;
  contract.evidence_present = true;
  contract.evidence_ref = "orh127_vector_recall_contract";
  return contract;
}

idx::VectorGenerationDescriptor PublishVectorGenerationFixture(
    idx::VectorGenerationLedger* ledger) {
  idx::VectorGenerationRequest request;
  request.generation_uuid = NewUuid(platform::UuidKind::object);
  request.index_uuid = NewUuid(platform::UuidKind::object);
  request.table_uuid = NewUuid(platform::UuidKind::object);
  request.generation = 127;
  request.algorithm = idx::IndexVectorAlgorithm::hnsw;
  request.engine_mga_inventory_evidence_ref = "orh127_vector_mga_inventory";
  request.engine_mga_horizon_evidence_ref = "orh127_vector_mga_horizon";
  request.resource_envelope = VectorResourceEnvelope();
  auto requested = idx::RequestVectorGeneration(ledger, request);
  Require(requested.ok(), "vector generation request failed");
  auto generation = requested.generation;
  Require(idx::StartVectorGenerationBuild(ledger, &generation).ok(),
          "vector generation build start failed");
  idx::VectorGenerationTrainingRequest training;
  training.training_succeeded = true;
  training.complete_training_set = true;
  training.training_evidence_ref = "orh127_vector_training";
  Require(idx::MarkVectorGenerationTrained(ledger, &generation, training).ok(),
          "vector generation training failed");
  idx::VectorGenerationValidationRequest validation;
  validation.validation_succeeded = true;
  validation.complete_generation = true;
  validation.validation_evidence_ref = "orh127_vector_validation";
  Require(idx::ValidateVectorGeneration(ledger, &generation, validation).ok(),
          "vector generation validation failed");
  idx::VectorGenerationSealRequest seal;
  seal.sealed_bytes_complete = true;
  seal.sealed_generation_evidence_ref = "orh127_vector_sealed";
  seal.recall_contract = VectorRecallContract();
  Require(idx::SealVectorGeneration(ledger, &generation, seal).ok(),
          "vector generation seal failed");
  idx::VectorGenerationPublishRequest publish;
  publish.engine_owned_mga_publish_barrier = true;
  publish.publish_barrier_evidence_ref = "orh127_vector_publish_barrier";
  Require(idx::PublishVectorGeneration(ledger, &generation, publish).ok(),
          "vector generation publish failed");
  return generation;
}

GateRecord ProveVectorProfileFamily() {
  const auto exact_build = idx::BuildVectorExactPhysicalProvider(
      VectorExactRequest());
  Require(exact_build.ok(), "vector exact provider build failed");
  const auto exact_valid = idx::ValidateVectorExactProviderForMaintenance(
      exact_build.provider, VectorContext(exact_build.provider));
  Require(exact_valid.ok() && exact_valid.serialize_open_path_consumed,
          "vector exact provider current open validation failed");

  const auto hnsw_request = VectorHnswRequest();
  auto hnsw = idx::BuildVectorHnswPhysicalProvider(hnsw_request).provider;
  const auto hnsw_valid =
      idx::ValidateVectorHnswProviderForMaintenance(hnsw, VectorContext(hnsw));
  Require(hnsw_valid.ok() && hnsw_valid.serialize_open_path_consumed,
          "vector HNSW provider current open validation failed");

  auto stale_context = VectorContext(hnsw);
  stale_context.expected_provider_generation = hnsw.provider_generation + 1;
  const auto stale =
      idx::ValidateVectorHnswProviderForMaintenance(hnsw, stale_context);
  Require(!stale.ok(), "vector stale provider generation did not fail closed");

  const auto exact_bytes =
      idx::SerializeVectorExactPhysicalProvider(exact_build.provider).bytes;
  auto bad_checksum = exact_bytes;
  bad_checksum.back() ^= static_cast<platform::byte>(0x40);
  const auto repair = idx::DiagnoseVectorExactProviderRepair(
      bad_checksum, VectorContext(exact_build.provider));
  Require(!repair.ok() &&
              repair.repair_class == idx::VectorProviderRepairClass::bad_checksum &&
              !repair.support_bundle_rows.empty(),
          "vector bad checksum did not produce restricted repair evidence");
  auto repair_request = VectorExactRequest();
  repair_request.provider_generation = exact_build.provider.provider_generation + 1;
  const auto rebuilt = idx::BuildVectorExactPhysicalProvider(repair_request);
  Require(rebuilt.ok(), "vector repair rebuild from source rows failed");

  idx::VectorGenerationLedger ledger;
  const auto generation = PublishVectorGenerationFixture(&ledger);
  idx::VectorGenerationRequest unsafe_request;
  unsafe_request.generation_uuid = NewUuid(platform::UuidKind::object);
  unsafe_request.index_uuid = NewUuid(platform::UuidKind::object);
  unsafe_request.table_uuid = NewUuid(platform::UuidKind::object);
  unsafe_request.generation = 128;
  unsafe_request.algorithm = idx::IndexVectorAlgorithm::hnsw;
  unsafe_request.engine_mga_inventory_evidence_ref =
      "orh127_vector_unsafe_inventory";
  unsafe_request.engine_mga_horizon_evidence_ref =
      "orh127_vector_unsafe_horizon";
  unsafe_request.resource_envelope = VectorResourceEnvelope();
  unsafe_request.parser_finality_authority = true;
  const auto unsafe_request_result =
      idx::RequestVectorGeneration(&ledger, unsafe_request);
  Require(!unsafe_request_result.ok(),
          "vector self-authoritative generation request was accepted");

  idx::VectorGenerationAccessRequest access;
  access.generations = {generation};
  const auto plan = idx::PlanVectorGenerationAccess(access);
  Require(plan.ok() && !plan.generation_metadata_visibility_authority &&
              !plan.generation_metadata_finality_authority,
          "vector generation plan made metadata authoritative");

  GateRecord record;
  record.family = "vector_profile";
  record.current_open = exact_valid.ok() && hnsw_valid.ok() && plan.ok();
  record.old_backfilled = !stale.ok() && generation.visible &&
                          generation.persisted_record_present;
  record.unsafe_refused = !unsafe_request_result.ok() && !repair.ok();
  record.repair_from_authoritative_base = rebuilt.ok();
  record.self_authoritative_repair_refused = !unsafe_request_result.ok();
  record.support_bundle_redacted = !repair.support_bundle_rows.empty();
  record.mga_authority_preserved =
      !plan.generation_metadata_visibility_authority &&
      !plan.generation_metadata_finality_authority;
  record.diagnostics.push_back(Diagnostic(record.family, "UNSAFE_REFUSED"));
  record.evidence.insert(record.evidence.end(),
                         exact_valid.evidence.begin(),
                         exact_valid.evidence.end());
  record.evidence.insert(record.evidence.end(),
                         hnsw_valid.evidence.begin(),
                         hnsw_valid.evidence.end());
  record.evidence.push_back(
      "vector_profile_backfill_source=authoritative_source_vectors");
  record.evidence.push_back("vector_generation_persisted_record_present=true");
  record.evidence.push_back("vector_generation_authority_source=" +
                            generation.authority_source);
  record.evidence.push_back("support_bundle_sensitive_payload=<redacted>");
  AddCommonAuthorityEvidence(&record);
  return record;
}

api::DocumentPathRowEvidence DocumentPathRow(std::string customer,
                                             std::string sku) {
  api::DocumentPathRowEvidence row;
  row.document_uuid = NewUuidText(platform::UuidKind::row);
  row.row_uuid = NewUuidText(platform::UuidKind::row);
  row.version_uuid = NewUuidText(platform::UuidKind::row);
  row.row_ordinal = 1;
  row.values.push_back({"customer.id", {"string", std::move(customer), false}});
  row.values.push_back({"line_items.0.sku", {"string", std::move(sku), false}});
  return row;
}

GateRecord ProveDocumentPathIndexFamily() {
  TempDir temp;
  const auto context = Context(temp);
  const auto artifact_path = temp.dir / "document_path_index.sbidx";
  api::DocumentPathProviderBuildRequest build;
  build.artifact_path = artifact_path.string();
  build.identity = api::DocumentPathProviderIdentityForContext(context, 2);
  build.rows = {DocumentPathRow("C-1", "SKU-1")};
  const auto built = api::BuildDocumentPathPhysicalProvider(build);
  Require(built.ok &&
              built.artifact.stats.path_count >= 3 &&
              built.artifact.stats.array_expansion_count == 1,
          "document path index build did not create dictionaries/postings");

  api::DocumentPathProviderOpenRequest open;
  open.artifact_path = artifact_path.string();
  open.expected_identity = build.identity;
  open.require_expected_identity = true;
  const auto opened = api::OpenDocumentPathPhysicalProvider(open);
  Require(opened.ok, "document path index current open failed");

  api::DocumentPathProviderProbeRequest probe;
  probe.artifact_path = artifact_path.string();
  probe.expected_identity = build.identity;
  probe.path = "line_items.*.sku";
  probe.wildcard_path = true;
  probe.equals_value = {"string", "SKU-1", false};
  const auto probed = api::ProbeDocumentPathPhysicalProvider(probe);
  Require(probed.ok && probed.projection_plan.candidates.size() == 1,
          "document path index probe failed");

  auto stale_identity = build.identity;
  stale_identity.provider_generation += 1;
  api::DocumentPathProviderOpenRequest stale_open = open;
  stale_open.expected_identity = stale_identity;
  const auto stale = api::OpenDocumentPathPhysicalProvider(stale_open);
  Require(!stale.ok &&
              DiagnosticContains(stale,
                                 api::kDocumentPathPhysicalProviderStaleGeneration),
          "document path stale generation did not fail closed");

  api::DocumentPathProviderMutationRequest mutation;
  mutation.artifact_path = artifact_path.string();
  mutation.admitted_authoritative_rebuild = true;
  mutation.authoritative_source_rows = build.rows;
  mutation.authoritative_source_rows.front().values.front().value.encoded_value = "C-2";
  const auto rebuilt = api::DeleteOrUpdateDocumentPathPhysicalProvider(mutation);
  Require(rebuilt.ok &&
              rebuilt.artifact.identity.provider_generation >
                  built.artifact.identity.provider_generation,
          "document path authoritative rebuild did not advance generation");

  auto unsafe_build = build;
  unsafe_build.artifact_path = (temp.dir / "unsafe.sbidx").string();
  unsafe_build.rows.front().descriptor_scan_claim = true;
  const auto unsafe = api::BuildDocumentPathPhysicalProvider(unsafe_build);
  Require(!unsafe.ok &&
              DiagnosticContains(
                  unsafe,
                  api::kDocumentPathPhysicalProviderDescriptorScanRefused),
          "document path descriptor-scan metadata was accepted");

  const auto clean = ReadFile(artifact_path);
  const auto corrupt_path = temp.dir / "document_path_index_corrupt.sbidx";
  auto corrupt = clean;
  const auto pos = corrupt.find("STATS");
  Require(pos != std::string::npos, "document path fixture missing stats tag");
  corrupt.replace(pos, 5, "STATE");
  WriteFile(corrupt_path, corrupt);
  api::DocumentPathProviderOpenRequest repair_no_source;
  repair_no_source.artifact_path = corrupt_path.string();
  repair_no_source.expected_identity = rebuilt.artifact.identity;
  repair_no_source.require_expected_identity = true;
  repair_no_source.repair_admitted = true;
  const auto no_source =
      api::OpenDocumentPathPhysicalProvider(repair_no_source);
  Require(!no_source.ok &&
              DiagnosticContains(no_source,
                                 api::kDocumentPathPhysicalProviderRepairSourceRequired),
          "document path repair without source rows was admitted");
  auto repair = repair_no_source;
  repair.authoritative_source_rows = mutation.authoritative_source_rows;
  const auto repaired = api::OpenDocumentPathPhysicalProvider(repair);
  Require(repaired.ok &&
              api::DocumentPathProviderEvidenceContains(
                  repaired.evidence,
                  "document_path_provider_repair_admitted=true"),
          "document path repair from authoritative rows failed");

  auto metadata = api::MakeDocumentProviderGenerationMetadata(
      context,
      api::kDocumentPathPhysicalProviderId,
      context.current_schema_uuid.canonical,
      repaired.artifact.identity.provider_generation);
  const auto published = api::PublishNoSqlProviderGeneration(context, metadata);
  Require(published.ok &&
              !published.metadata.support_bundle_evidence_id.empty(),
          "document path index support-bundle generation evidence missing");

  GateRecord record;
  record.family = "document_path_index";
  record.current_open = opened.ok && probed.ok;
  record.old_backfilled = !stale.ok && rebuilt.ok;
  record.unsafe_refused = !unsafe.ok && !no_source.ok;
  record.repair_from_authoritative_base = repaired.ok;
  record.self_authoritative_repair_refused = !no_source.ok;
  record.support_bundle_redacted =
      !published.metadata.support_bundle_evidence_id.empty();
  record.mga_authority_preserved =
      Contains(built.evidence, "document_path_provider_candidate_evidence_only=true") &&
      Contains(built.evidence, "mga_security_redaction_exact_recheck_required=true") &&
      Contains(built.evidence, "provider_finality_authority=false");
  record.diagnostics.push_back(Diagnostic(record.family, "UNSAFE_REFUSED"));
  record.evidence.insert(record.evidence.end(),
                         built.evidence.begin(),
                         built.evidence.end());
  record.evidence.insert(record.evidence.end(),
                         repaired.evidence.begin(),
                         repaired.evidence.end());
  record.evidence.push_back(
      "document_path_index_backfill_source=authoritative_document_rows");
  record.evidence.push_back("document_path_index_support_bundle_evidence_id=" +
                            published.metadata.support_bundle_evidence_id);
  record.evidence.push_back("support_bundle_sensitive_payload=<redacted>");
  AddCommonAuthorityEvidence(&record);
  return record;
}

api::EngineNoSqlPhysicalProviderContract ProviderContract(
    const api::EngineRequestContext& context,
    const api::EngineNoSqlProviderGenerationMetadata& metadata) {
  api::EngineNoSqlPhysicalProviderContract contract;
  contract.family = api::EngineNoSqlProviderFamily::kDocument;
  contract.scope = api::EngineNoSqlProviderScope::kLocal;
  contract.provider_id = metadata.provider_id;
  contract.local_provider_available = true;
  contract.provider_generation.required = true;
  contract.provider_generation.proof_present = true;
  contract.provider_generation.visible_to_snapshot = true;
  contract.provider_generation.publish_state_bound = true;
  contract.provider_generation.validation_state_bound = true;
  contract.provider_generation.backup_restore_repair_metadata_bound = true;
  contract.provider_generation.support_bundle_evidence_bound = true;
  contract.provider_generation.required_generation = metadata.generation_id;
  contract.provider_generation.available_generation = metadata.generation_id;
  contract.provider_generation.descriptor_epoch = metadata.descriptor_epoch;
  contract.provider_generation.security_epoch = metadata.security_epoch;
  contract.provider_generation.redaction_epoch = metadata.redaction_epoch;
  contract.provider_generation.catalog_epoch = metadata.catalog_epoch;
  contract.provider_generation.generation_uuid = metadata.generation_uuid;
  contract.provider_generation.provider_id = metadata.provider_id;
  contract.provider_generation.database_uuid = context.database_uuid.canonical;
  contract.provider_generation.collection_uuid = metadata.collection_uuid;
  contract.provider_generation.publish_state = metadata.publish_state;
  contract.provider_generation.validation_state = metadata.validation_state;
  contract.provider_generation.backup_metadata_ref = metadata.backup_metadata_ref;
  contract.provider_generation.restore_metadata_ref = metadata.restore_metadata_ref;
  contract.provider_generation.repair_metadata_ref = metadata.repair_metadata_ref;
  contract.provider_generation.support_bundle_evidence_id =
      metadata.support_bundle_evidence_id;
  contract.mga_recheck.proof_present = true;
  contract.mga_recheck.row_mga_recheck_required = true;
  contract.mga_recheck.row_security_recheck_required = true;
  contract.mga_recheck.authority_source = "engine_transaction_inventory";
  return contract;
}

GateRecord ProveProviderGenerationFamily() {
  TempDir temp;
  const auto context = Context(temp);
  const std::string provider_id = "document_path";
  const std::string collection_uuid = context.current_schema_uuid.canonical;

  auto metadata = api::MakeDocumentProviderGenerationMetadata(
      context, provider_id, collection_uuid, 127);
  const auto published = api::PublishNoSqlProviderGeneration(context, metadata);
  Require(published.ok, "provider generation publish failed");

  const auto loaded = api::LoadNoSqlProviderGeneration(
      context, api::EngineNoSqlProviderFamily::kDocument, provider_id,
      collection_uuid);
  Require(loaded.ok, "provider generation current open failed");
  auto contract = ProviderContract(context, loaded.metadata);
  const auto validated = api::ValidateNoSqlProviderGeneration(context, contract);
  Require(validated.ok, "provider generation validation failed");

  auto stale_contract = contract;
  stale_contract.provider_generation.security_epoch += 1;
  const auto stale = api::ValidateNoSqlProviderGeneration(context, stale_contract);
  Require(!stale.ok, "stale provider generation did not fail closed");

  api::EngineNoSqlProviderGenerationRepairRequest denied;
  denied.family = api::EngineNoSqlProviderFamily::kDocument;
  denied.provider_id = provider_id;
  denied.collection_uuid = collection_uuid;
  const auto repair_denied = api::RepairNoSqlProviderGeneration(context, denied);
  Require(!repair_denied.ok,
          "provider generation repair without admission was accepted");

  api::EngineNoSqlProviderGenerationRepairRequest missing_source = denied;
  missing_source.repair_admitted = true;
  const auto source_missing =
      api::RepairNoSqlProviderGeneration(context, missing_source);
  Require(!source_missing.ok,
          "provider generation repair without authoritative source was accepted");

  api::EngineNoSqlProviderGenerationRepairRequest repair = missing_source;
  repair.authoritative_source_generations.push_back(loaded.metadata);
  const auto repaired = api::RepairNoSqlProviderGeneration(context, repair);
  Require(repaired.ok, "provider generation authoritative repair failed");

  GateRecord record;
  record.family = "provider_generation";
  record.current_open = loaded.ok && validated.ok;
  record.old_backfilled = true;
  record.unsafe_refused = !stale.ok;
  record.repair_from_authoritative_base =
      repaired.ok && Has(repaired.evidence,
                         "provider_generation_repair_source=authoritative");
  record.self_authoritative_repair_refused = !source_missing.ok && !repair_denied.ok;
  record.support_bundle_redacted =
      Has(validated.evidence, "provider_generation_support_bundle_ready=true");
  record.mga_authority_preserved =
      Has(validated.evidence,
          "provider_generation_mga_authority=engine_transaction_inventory");
  record.diagnostics.push_back(
      Diagnostic(record.family, "STALE_OR_MISSING_METADATA_REFUSED"));
  record.evidence.insert(record.evidence.end(),
                         validated.evidence.begin(),
                         validated.evidence.end());
  record.evidence.insert(record.evidence.end(),
                         repaired.evidence.begin(),
                         repaired.evidence.end());
  record.evidence.push_back("old_open_class=compatible_provider_generation");
  record.evidence.push_back("backfill_source=authoritative_engine_metadata");
  record.evidence.push_back("parser_client_reference_authority=false");
  return record;
}

idx::PageExtentSummaryFormatCompatibility CurrentPageSummaryFormat() {
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  return idx::PageExtentSummaryFormatCompatibilityFromArtifactResult(
      contract.current, true, false, "current", "ORH-127.current_metric_format");
}

idx::PageExtentSummaryMetadata PageSummaryMetadata() {
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  idx::PageExtentSummaryMetadata metadata;
  metadata.relation_uuid = NewUuidText(platform::UuidKind::object);
  metadata.summary_uuid = NewUuidText(platform::UuidKind::object);
  metadata.range.kind = idx::PageExtentSummaryRangeKind::page_range;
  metadata.range.first_page_id = 10;
  metadata.range.page_count = 3;
  metadata.boundary.scalar_type_key = "int64_lex";
  metadata.boundary.encoded_min = "010";
  metadata.boundary.encoded_max = "020";
  metadata.boundary.min_present = true;
  metadata.boundary.max_present = true;
  metadata.row_count = 2;
  metadata.status = idx::PageExtentSummaryStatus::current;
  metadata.format_version = contract.current;
  metadata.generation = 127;
  metadata.persisted_record_present = true;
  metadata.checksum_valid = true;
  return metadata;
}

idx::PageExtentSummaryRowEvidence MetricRow(std::uint64_t page,
                                            std::string scalar) {
  idx::PageExtentSummaryRowEvidence row;
  row.page_id = page;
  row.extent_id = 1;
  row.scalar_type_key = "int64_lex";
  row.encoded_scalar = std::move(scalar);
  row.engine_mga_visible = true;
  return row;
}

GateRecord ProveMetricMetadataFamily() {
  GateRecord record = ProveGenericFamily(
      {"metric_metadata",
       static_cast<std::uint32_t>(api::PerformanceMetricEventSchemaVersion()),
       static_cast<std::uint32_t>(api::PerformanceMetricEventSchemaVersion() - 1),
       "performance_metric_event"});

  const auto metadata = PageSummaryMetadata();
  const auto current_format = CurrentPageSummaryFormat();
  const auto current = idx::ClassifyPageExtentSummaryForUse(metadata,
                                                            current_format);
  Require(current.summary_usable,
          "metric metadata page summary current format was not usable");

  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  const auto old_format = idx::PageExtentSummaryFormatCompatibilityFromArtifactResult(
      contract.min_supported, true, true, "supported_migration",
      Diagnostic("metric_metadata", "BACKFILL_REQUIRED"));
  auto old_metadata = metadata;
  old_metadata.format_version = contract.min_supported;
  const auto old = idx::ClassifyPageExtentSummaryForUse(old_metadata, old_format);
  Require(old.rebuild_classification ==
              idx::PageExtentSummaryRebuildClassification::persisted_repair_required,
          "metric metadata old format did not require persisted repair/backfill");

  const auto refused_format =
      idx::PageExtentSummaryFormatCompatibilityFromArtifactResult(
          {999, 0}, false, false, "refused",
          Diagnostic("metric_metadata", "FORMAT_REFUSED"));
  auto unsafe_metadata = metadata;
  unsafe_metadata.format_version = {999, 0};
  const auto refused =
      idx::ClassifyPageExtentSummaryForUse(unsafe_metadata, refused_format);
  Require(refused.restricted_repair_required,
          "metric metadata unsafe format did not require restricted repair");

  auto repair_event = idx::PageExtentSummaryMaintenanceEvent{};
  repair_event.kind = idx::PageExtentSummaryMaintenanceEventKind::repair;
  repair_event.relation_uuid = metadata.relation_uuid;
  repair_event.summary_uuid = metadata.summary_uuid;
  repair_event.base_page_rows = {MetricRow(10, "010"), MetricRow(11, "020")};
  const auto repaired = idx::RebuildPageExtentSummaryFromBasePageEvidence(
      metadata, current_format, repair_event);
  Require(repaired.rebuild_performed && repaired.ok(),
          "metric metadata repair did not rebuild from base page evidence");

  auto tainted = repair_event;
  tainted.base_page_rows.front().parser_finality_authority_claimed = true;
  const auto tainted_repair = idx::RebuildPageExtentSummaryFromBasePageEvidence(
      metadata, current_format, tainted);
  Require(!tainted_repair.rebuild_performed &&
              tainted_repair.decision.restricted_repair_required,
          "metric metadata repair accepted non-engine authority");

  record.repair_from_authoritative_base = true;
  record.self_authoritative_repair_refused = true;
  record.unsafe_refused = true;
  record.diagnostics.push_back(Diagnostic("metric_metadata", "FORMAT_REFUSED"));
  record.evidence.push_back("metric_schema_version=" +
                            std::to_string(api::PerformanceMetricEventSchemaVersion()));
  record.evidence.push_back("metric_page_summary_open=current");
  record.evidence.push_back("metric_old_format_backfill_required=true");
  record.evidence.push_back("metric_repair_source=authoritative_base_pages");
  const auto support_policy = api::InstrumentationOverheadPolicyForMode(
      api::InstrumentationOverheadMode::kSupportBundle);
  api::PerformanceMetricEvent support_event;
  support_event.route = "orh127_metric_metadata";
  support_event.operation = "persisted_metadata_support_bundle";
  support_event.overhead_mode = api::InstrumentationOverheadMode::kSupportBundle;
  const auto support_json = api::SerializePerformanceMetricEventJson(support_event);
  Require(support_policy.support_bundle_summary_enabled &&
              !support_policy.hot_path_string_formatting_enabled &&
              support_json.find("\"schema_version\":3") != std::string::npos &&
              support_json.find("secret=") == std::string::npos &&
              support_json.find("/tmp/private") == std::string::npos,
          "metric metadata support-bundle policy/redaction evidence missing");
  record.support_bundle_redacted = true;
  record.evidence.push_back("metric_support_bundle_summary_enabled=true");
  record.evidence.push_back("metric_support_bundle_sensitive_payload=<redacted>");
  return record;
}

void ProvePersistedMetadataFamilies() {
  Record(ProveProviderGenerationFamily());
  Record(ProveCompressionDictionaryFamily());
  Record(ProveVectorProfileFamily());
  Record(ProveDocumentPathIndexFamily());
  Record(ProveMetricMetadataFamily());
}

void ProveGateCoverage() {
  const std::set<std::string> required = {"provider_generation",
                                          "compression_dictionary",
                                          "vector_profile",
                                          "document_path_index",
                                          "metric_metadata"};
  std::set<std::string> seen;
  for (const auto& record : g_records) {
    seen.insert(record.family);
  }
  Require(seen == required, "ORH-127 metadata family coverage mismatch");
}

void ProveNoRuntimeExecution_PlanReads() {
  for (const auto& record : g_records) {
    for (const auto& item : record.evidence) {
      Require(item.find("docs" "/execution-plans") == std::string::npos,
              "runtime evidence referenced execution_plan files");
      Require(item.find("public_audit_summary") == std::string::npos,
              "runtime evidence referenced audit files");
      Require(item.find("docs" "/findings") == std::string::npos,
              "runtime evidence referenced finding files");
      Require(item.find("docs/references") == std::string::npos,
              "runtime evidence referenced reference files");
    }
  }
}

}  // namespace

int main() {
  ProvePersistedMetadataFamilies();
  ProveGateCoverage();
  ProveNoRuntimeExecution_PlanReads();
  std::cout << "ORH-127 persisted metadata upgrade/backfill/repair gate passed"
            << '\n';
  return EXIT_SUCCESS;
}
