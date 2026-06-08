// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_fault_injection_matrix.hpp"
#include "crud_support/crud_store.hpp"
#include "nosql/document_path_physical_provider.hpp"
#include "nosql/nosql_provider_generation_store.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace api = scratchbird::engine::internal_api;
namespace idx = scratchbird::core::index;

namespace {

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "index_fault_injection_crash_matrix_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(const std::vector<std::string>& values, std::string_view value) {
  return std::any_of(values.begin(), values.end(), [&](const auto& item) {
    return item == value || item.find(value) != std::string::npos;
  });
}

bool HasMarker(const std::string& value) {
  static const std::vector<std::string> markers = {
      "docs" "/execution-plans", "public_release_evidence", "docs" "/findings",
      "docs/reference", "execution_plan", "findings", "contracts"};
  return std::any_of(markers.begin(), markers.end(), [&](const auto& marker) {
    return value.find(marker) != std::string::npos;
  });
}

const idx::IndexFaultInjectionMatrixRow& FindRow(
    const std::vector<idx::IndexFaultInjectionMatrixRow>& rows,
    std::string_view surface) {
  const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
    return row.surface == surface;
  });
  if (found == rows.end()) {
    Fail("missing matrix row " + std::string(surface));
  }
  return *found;
}

void RequireNoAuthority(const idx::IndexFaultInjectionMatrixRow& row) {
  Require(!row.parser_authority, row.surface + " claimed parser authority");
  Require(!row.donor_authority, row.surface + " claimed donor authority");
  Require(!row.provider_authority, row.surface + " claimed provider authority");
  Require(!row.storage_authority, row.surface + " claimed storage authority");
  Require(!row.visibility_authority,
          row.surface + " claimed visibility authority");
  Require(!row.security_authority, row.surface + " claimed security authority");
  Require(!row.transaction_finality_authority,
          row.surface + " claimed transaction finality authority");
  Require(!row.recovery_authority, row.surface + " claimed recovery authority");
  Require(Contains(row.evidence, "parser_authority=false"),
          row.surface + " missing parser non-authority evidence");
  Require(Contains(row.evidence, "transaction_finality_authority=false"),
          row.surface + " missing finality non-authority evidence");
}

void RequireNoRuntimePathLeak(const idx::IndexFaultInjectionMatrixRow& row) {
  Require(row.runtime_dependency_free,
          row.surface + " runtime dependency flag was false");
  Require(!HasMarker(row.surface), row.surface + " leaked marker in surface");
  Require(!HasMarker(row.fault_point),
          row.surface + " leaked marker in fault point");
  Require(!HasMarker(row.expected_action),
          row.surface + " leaked marker in action");
  for (const auto& evidence : row.evidence) {
    Require(!HasMarker(evidence), row.surface + " leaked marker in evidence");
  }
}

void RequireCapabilityBlocker(
    const idx::IndexFaultInjectionMatrixRow& row) {
  const auto family = idx::FindBuiltinIndexFamilyById(row.family_id);
  Require(family.ok(), row.surface + " family lookup failed");
  const auto* state =
      idx::FindBuiltinIndexFamilyPhysicalCapabilityState(family.descriptor->family);
  Require(state != nullptr, row.surface + " missing capability state");
  if (state->runtime_available) {
    return;
  }
  Require(row.refused, row.surface + " incomplete family was not refused");
  Require(row.fail_closed, row.surface + " incomplete family did not fail closed");
  Require(!row.recovered, row.surface + " incomplete family counted recovered");
  Require(!row.planner_visible,
          row.surface + " incomplete family became planner visible");
  if (family.descriptor->persistence == idx::IndexPersistenceClass::donor_emulated) {
    Require(row.diagnostic_code ==
                "IRC.INDEX_REPAIR.DONOR_EMULATED.NON_AUTHORITY_MAPPING",
            row.surface + " donor diagnostic mismatch");
    return;
  }
  Require(row.diagnostic_code == state->blocker_diagnostic_code,
          row.surface + " blocker diagnostic mismatch: " + row.diagnostic_code);
  Require(row.message_key == state->blocker_message_key,
          row.surface + " blocker message mismatch");
}

api::EngineRequestContext Context(const std::filesystem::path& path) {
  api::EngineRequestContext context;
  context.database_path = path.string();
  context.database_uuid.canonical = api::GenerateCrudEngineUuid("database");
  context.current_schema_uuid.canonical = api::GenerateCrudEngineUuid("schema");
  context.local_transaction_id = 181;
  context.transaction_uuid.canonical = api::GenerateCrudEngineUuid("transaction");
  context.catalog_generation_id = 701;
  context.security_epoch = 702;
  context.resource_epoch = 703;
  context.security_context_present = true;
  return context;
}

api::DocumentPathProviderIdentity DocumentIdentity(
    const api::EngineRequestContext& context,
    std::uint64_t generation) {
  auto identity =
      api::DocumentPathProviderIdentityForContext(context, generation);
  identity.relation_uuid = api::GenerateCrudEngineUuid("relation");
  identity.index_uuid = api::GenerateCrudEngineUuid("index");
  identity.segment_uuid = api::GenerateCrudEngineUuid("segment");
  return identity;
}

api::DocumentPathRowEvidence DocumentRow(std::uint64_t ordinal) {
  api::DocumentPathRowEvidence row;
  row.document_uuid = api::GenerateCrudEngineUuid("row");
  row.row_uuid = api::GenerateCrudEngineUuid("row");
  row.version_uuid = api::GenerateCrudEngineUuid("row");
  row.row_ordinal = ordinal;
  api::DocumentPathValueEvidence value;
  value.path = "tenant.status";
  value.value.scalar_type = "string";
  value.value.encoded_value = "active";
  row.values.push_back(value);
  return row;
}

void DocumentProviderGenerationUsesCurrentHelpers() {
  const auto temp = std::filesystem::temp_directory_path() /
                    "scratchbird_irc181_document_provider.sbdb";
  std::filesystem::remove(temp);
  std::filesystem::remove(temp.string() + ".sb.nosql_provider_generations");
  std::filesystem::remove(temp.string() + ".document_path_provider");

  const auto context = Context(temp);
  const std::string collection_uuid =
      api::GenerateCrudEngineUuid("collection");
  const auto metadata = api::MakeDocumentProviderGenerationMetadata(
      context, api::kDocumentPathPhysicalProviderId, collection_uuid, 1);
  const auto published = api::PublishNoSqlProviderGeneration(context, metadata);
  Require(published.ok, "document generation publish failed: " +
                            published.diagnostic.code);
  const auto loaded = api::LoadNoSqlProviderGeneration(
      context,
      api::EngineNoSqlProviderFamily::kDocument,
      api::kDocumentPathPhysicalProviderId,
      collection_uuid);
  Require(loaded.ok, "document generation load failed: " +
                         loaded.diagnostic.code);
  Require(!loaded.metadata.provider_claims_transaction_finality_authority,
          "document generation claimed finality authority");
  Require(!loaded.metadata.provider_claims_visibility_authority,
          "document generation claimed visibility authority");

  const auto artifact_path = api::DocumentPathPhysicalProviderPath(context);
  api::DocumentPathProviderBuildRequest build;
  build.artifact_path = artifact_path;
  build.identity = DocumentIdentity(context, 1);
  build.rows = {DocumentRow(1), DocumentRow(2)};
  const auto built = api::BuildDocumentPathPhysicalProvider(build);
  Require(built.ok, "document provider build failed: " + built.diagnostic.code);

  api::DocumentPathProviderOpenRequest open;
  open.artifact_path = artifact_path;
  open.expected_identity = build.identity;
  open.require_expected_identity = true;
  const auto opened = api::OpenDocumentPathPhysicalProvider(open);
  Require(opened.ok, "document provider open failed: " + opened.diagnostic.code);
  Require(Contains(opened.evidence, "document_path_provider_opened=true"),
          "document provider clean reopen evidence missing");

  std::ifstream in(artifact_path, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
  Require(!bytes.empty(), "document provider artifact missing");
  if (bytes.size() > 32) {
    bytes[32] = static_cast<char>(bytes[32] ^ 0x5a);
  }
  std::ofstream out(artifact_path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  out.close();
  const auto corrupt_open = api::OpenDocumentPathPhysicalProvider(open);
  Require(!corrupt_open.ok,
          "document provider corrupt open did not fail closed");
  Require(corrupt_open.diagnostic.code ==
              api::kDocumentPathPhysicalProviderBadChecksum ||
              corrupt_open.diagnostic.detail.find(
                  api::kDocumentPathPhysicalProviderBadChecksum) !=
                  std::string::npos,
          "document provider corrupt diagnostic mismatch: " +
              corrupt_open.diagnostic.code);
}

void MatrixRowsAreExecutableAndClosed() {
  const auto result = idx::BuildIndexFaultInjectionCrashMatrix();
  Require(result.ok(), "matrix build failed: " + result.diagnostic.diagnostic_code);
  Require(!result.rows.empty(), "matrix rows missing");

  std::set<std::string> row_keys;
  for (const auto& row : result.rows) {
    Require(row.concrete_execution_result,
            row.surface + " did not execute a native helper");
    Require(!row.diagnostic_code.empty(), row.surface + " missing diagnostic");
    Require(!row.message_key.empty(), row.surface + " missing message key");
    Require(row.refused != row.recovered,
            row.surface + " must be exactly recovered or refused");
    RequireNoAuthority(row);
    RequireNoRuntimePathLeak(row);
    row_keys.insert(row.surface + ":" + row.family_id + ":" + row.fault_point);
  }
  Require(row_keys.size() == result.rows.size(), "duplicate matrix rows found");

  const auto& btree = FindRow(result.rows, "btree_insert_split_root_publish");
  Require(Contains(btree.evidence, "btree.root_split_observed=true"),
          "btree split evidence missing");
  RequireCapabilityBlocker(btree);

  const auto& bulk = FindRow(result.rows, "sorted_bulk_root_publish");
  Require(bulk.old_or_new_validated_root_only,
          "bulk recovery did not prove old/new root rule");
  Require(bulk.exactly_one_visible_generation,
          "bulk recovery did not prove one visible generation");
  RequireCapabilityBlocker(bulk);

  const auto& unsafe = result.rows.at(
      static_cast<std::size_t>(std::distance(
          result.rows.begin(),
          std::find_if(result.rows.begin(), result.rows.end(), [](const auto& row) {
            return row.surface == "sorted_bulk_root_publish" &&
                   row.fault_point.find("without durable fence") !=
                       std::string::npos;
          }))));
  Require(unsafe.refused && unsafe.fail_closed,
          "unsafe half-published bulk row did not fail closed");
  Require(unsafe.unsafe_half_published_state_refused,
          "unsafe half-published flag missing");

  const auto& delta = FindRow(result.rows, "secondary_delta_ledger");
  Require(delta.recovered, "delta ledger recovery refused");
  Require(Contains(delta.evidence,
                   "delta_ledger.recovery_action=apply_overlay_then_merge"),
          "delta ledger action evidence missing");

  RequireCapabilityBlocker(FindRow(result.rows, "text_segment_publish"));
  RequireCapabilityBlocker(FindRow(result.rows, "vector_exact_provider_publish"));
  RequireCapabilityBlocker(FindRow(result.rows, "vector_hnsw_provider_publish"));
  RequireCapabilityBlocker(FindRow(result.rows, "vector_ivf_provider_publish"));
  RequireCapabilityBlocker(FindRow(result.rows, "compressed_bitmap_spill"));
  RequireCapabilityBlocker(FindRow(result.rows, "hash_split_merge_overflow"));
  RequireCapabilityBlocker(FindRow(result.rows, "document_provider_generation_open"));

  const auto& graph = FindRow(result.rows, "graph_provider_publish");
  Require(graph.recovered && !graph.fail_closed && graph.planner_visible,
          "complete graph provider did not recover");
  Require(graph.exactly_one_visible_generation,
          "graph provider generation evidence missing");

  for (const auto& descriptor : idx::BuiltinIndexFamilyDescriptors()) {
    const auto* state =
        idx::FindBuiltinIndexFamilyPhysicalCapabilityState(descriptor.family);
    Require(state != nullptr, "missing state for " + descriptor.id);
    if (state->runtime_available) {
      continue;
    }
    const bool covered = std::any_of(result.rows.begin(), result.rows.end(),
                                     [&](const auto& row) {
                                       return row.family_id == descriptor.id &&
                                              !row.capability_blocker.empty();
                                     });
    Require(covered, "missing incomplete-family blocker row for " +
                         descriptor.id);
  }
}

}  // namespace

int main() {
  MatrixRowsAreExecutableAndClosed();
  DocumentProviderGenerationUsesCurrentHelpers();
  std::cout << "index_fault_injection_crash_matrix_gate=passed\n";
  return EXIT_SUCCESS;
}
