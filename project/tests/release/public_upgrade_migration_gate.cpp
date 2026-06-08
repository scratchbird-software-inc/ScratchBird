// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ScratchBird contributors
//
// PUBLIC_UPGRADE_MIGRATION_GATE

#include "catalog_page.hpp"
#include "catalog_record_codec.hpp"
#include "cluster_catalog_schema_versioning.hpp"
#include "database_format.hpp"
#include "database_lifecycle.hpp"
#include "datatype_wire_metadata.hpp"
#include "index_family_registry.hpp"
#include "index_metapage.hpp"
#include "startup_state.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace catalog = scratchbird::core::catalog;
namespace datatype = scratchbird::core::datatypes;
namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace index = scratchbird::core::index;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

using platform::TypedUuid;
using platform::UuidKind;
using platform::u64;

struct EvidenceRow {
  std::string surface;
  std::string scenario;
  std::string decision;
  std::string diagnostic_code;
  std::string compatibility_class;
  std::string authority;
};

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, 1783600000000ull + offset);
  Require(generated.ok(), "PCR-136 UUID generation failed");
  return generated.value;
}

void AddRow(std::vector<EvidenceRow>* rows,
            std::string surface,
            std::string scenario,
            std::string decision,
            std::string diagnostic_code,
            std::string compatibility_class,
            std::string authority) {
  rows->push_back({std::move(surface),
                   std::move(scenario),
                   std::move(decision),
                   std::move(diagnostic_code),
                   std::move(compatibility_class),
                   std::move(authority)});
}

std::string CsvEscape(std::string_view value) {
  bool quote = false;
  for (const char ch : value) {
    if (ch == ',' || ch == '"' || ch == '\n' || ch == '\r') {
      quote = true;
      break;
    }
  }
  if (!quote) {
    return std::string(value);
  }
  std::string escaped = "\"";
  for (const char ch : value) {
    if (ch == '"') {
      escaped += "\"\"";
    } else {
      escaped.push_back(ch);
    }
  }
  escaped.push_back('"');
  return escaped;
}

void WriteEvidenceCsv(const std::filesystem::path& path,
                      const std::vector<EvidenceRow>& rows) {
  if (path.empty()) {
    return;
  }
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  Require(static_cast<bool>(output), "PCR-136 evidence CSV open failed");
  output << "surface,scenario,decision,diagnostic_code,compatibility_class,authority\n";
  for (const auto& row : rows) {
    output << CsvEscape(row.surface) << ','
           << CsvEscape(row.scenario) << ','
           << CsvEscape(row.decision) << ','
           << CsvEscape(row.diagnostic_code) << ','
           << CsvEscape(row.compatibility_class) << ','
           << CsvEscape(row.authority) << '\n';
  }
  Require(static_cast<bool>(output), "PCR-136 evidence CSV write failed");
}

db::DatabaseArtifactVersionCompatibilityRequest ArtifactRequest(
    std::string kind,
    platform::u32 format_major,
    platform::u32 format_minor,
    platform::u32 current_major,
    platform::u32 current_minor) {
  db::DatabaseArtifactVersionCompatibilityRequest request;
  request.artifact_kind = std::move(kind);
  request.format_major = format_major;
  request.format_minor = format_minor;
  request.min_supported_major = 1;
  request.min_supported_minor = 0;
  request.current_major = current_major;
  request.current_minor = current_minor;
  request.max_supported_major = current_major;
  request.max_supported_minor = current_minor;
  return request;
}

std::string ExpectedPlanId(const db::DatabaseArtifactVersionCompatibilityRequest& request) {
  return request.artifact_kind + "_v" +
         std::to_string(request.format_major) + "_" +
         std::to_string(request.format_minor) + "_to_v" +
         std::to_string(request.current_major) + "_" +
         std::to_string(request.current_minor) + "_explicit_plan_v1";
}

void RequireArtifactOk(const db::DatabaseArtifactCompatibilityResult& result,
                       db::DatabaseOpenCompatibilityClass expected_class,
                       bool migration_required,
                       std::string_view message) {
  Require(result.ok(), message);
  Require(result.compatibility_class == expected_class,
          "PCR-136 artifact compatibility class mismatch");
  Require(result.migration_required == migration_required,
          "PCR-136 artifact migration-required flag mismatch");
}

void RequireArtifactRefusal(const db::DatabaseArtifactCompatibilityResult& result,
                            db::DatabaseOpenCompatibilityClass expected_class,
                            std::string_view expected_code,
                            std::string_view message) {
  Require(!result.ok(), message);
  Require(result.compatibility_class == expected_class,
          "PCR-136 artifact refusal compatibility class mismatch");
  Require(result.diagnostic.diagnostic_code == expected_code,
          "PCR-136 artifact refusal diagnostic mismatch");
}

void CheckVersionedArtifactSurface(std::string_view surface,
                                   std::string_view artifact_kind,
                                   std::vector<EvidenceRow>* rows) {
  auto current = ArtifactRequest(std::string(artifact_kind), 1, 0, 1, 0);
  const auto current_result =
      db::ClassifyDatabaseArtifactVersionCompatibility(current);
  RequireArtifactOk(current_result,
                    db::DatabaseOpenCompatibilityClass::current,
                    false,
                    "PCR-136 current artifact surface refused");
  AddRow(rows,
         std::string(surface),
         "current_format",
         "accepted",
         "",
         db::DatabaseOpenCompatibilityClassName(current_result.compatibility_class),
         "engine_version_classifier");

  auto migration = ArtifactRequest(std::string(artifact_kind), 1, 0, 1, 1);
  const auto missing_plan =
      db::ClassifyDatabaseArtifactVersionCompatibility(migration);
  RequireArtifactRefusal(
      missing_plan,
      db::DatabaseOpenCompatibilityClass::migration_required_without_plan_refused,
      "ENGINE.DBLC_MIGRATION_REQUIRED_WITHOUT_PLAN",
      "PCR-136 migration without explicit plan was accepted");
  AddRow(rows,
         std::string(surface),
         "minor_upgrade_missing_plan",
         "fail_closed",
         missing_plan.diagnostic.diagnostic_code,
         db::DatabaseOpenCompatibilityClassName(missing_plan.compatibility_class),
         "engine_version_classifier");

  migration.migration_plan_id = ExpectedPlanId(migration);
  const auto supported =
      db::ClassifyDatabaseArtifactVersionCompatibility(migration);
  RequireArtifactOk(supported,
                    db::DatabaseOpenCompatibilityClass::supported_migration,
                    true,
                    "PCR-136 explicit migration plan was refused");
  AddRow(rows,
         std::string(surface),
         "minor_upgrade_explicit_plan",
         "supported_migration",
         "",
         db::DatabaseOpenCompatibilityClassName(supported.compatibility_class),
         "engine_version_classifier");

  auto old = ArtifactRequest(std::string(artifact_kind), 0, 0, 1, 0);
  const auto unsupported_old =
      db::ClassifyDatabaseArtifactVersionCompatibility(old);
  RequireArtifactRefusal(unsupported_old,
                         db::DatabaseOpenCompatibilityClass::unsupported_old,
                         "ENGINE.DBLC_MIGRATION_UNSUPPORTED_OLD_ARTIFACT",
                         "PCR-136 unsupported old artifact was accepted");
  AddRow(rows,
         std::string(surface),
         "unsupported_old",
         "fail_closed",
         unsupported_old.diagnostic.diagnostic_code,
         db::DatabaseOpenCompatibilityClassName(unsupported_old.compatibility_class),
         "engine_version_classifier");

  auto future = ArtifactRequest(std::string(artifact_kind), 2, 0, 1, 1);
  const auto future_result =
      db::ClassifyDatabaseArtifactVersionCompatibility(future);
  RequireArtifactRefusal(future_result,
                         db::DatabaseOpenCompatibilityClass::newer_than_supported_refused,
                         "ENGINE.DBLC_FORMAT_NEWER_THAN_SUPPORTED",
                         "PCR-136 future artifact was accepted");
  AddRow(rows,
         std::string(surface),
         "future_format",
         "fail_closed",
         future_result.diagnostic.diagnostic_code,
         db::DatabaseOpenCompatibilityClassName(future_result.compatibility_class),
         "engine_version_classifier");

  auto downgrade = ArtifactRequest(std::string(artifact_kind), 1, 0, 1, 0);
  downgrade.downgrade_requested = true;
  const auto downgrade_result =
      db::ClassifyDatabaseArtifactVersionCompatibility(downgrade);
  RequireArtifactRefusal(downgrade_result,
                         db::DatabaseOpenCompatibilityClass::downgrade_refused,
                         "ENGINE.DBLC_FORMAT_DOWNGRADE_REFUSED",
                         "PCR-136 downgrade artifact was accepted");
  AddRow(rows,
         std::string(surface),
         "downgrade_requested",
         "fail_closed",
         downgrade_result.diagnostic.diagnostic_code,
         db::DatabaseOpenCompatibilityClassName(downgrade_result.compatibility_class),
         "engine_version_classifier");

  auto ambiguous = ArtifactRequest(std::string(artifact_kind), 1, 0, 1, 0);
  ambiguous.identity_proven = false;
  const auto ambiguous_result =
      db::ClassifyDatabaseArtifactVersionCompatibility(ambiguous);
  RequireArtifactRefusal(ambiguous_result,
                         db::DatabaseOpenCompatibilityClass::ambiguous_identity_refused,
                         "ENGINE.DBLC_MIGRATION_AMBIGUOUS_IDENTITY_REFUSED",
                         "PCR-136 ambiguous artifact identity was accepted");
  AddRow(rows,
         std::string(surface),
         "ambiguous_identity",
         "fail_closed",
         ambiguous_result.diagnostic.diagnostic_code,
         db::DatabaseOpenCompatibilityClassName(ambiguous_result.compatibility_class),
         "engine_version_classifier");
}

catalog::CatalogTypedRecord MakeCatalogRecord() {
  catalog::CatalogTypedRecord record;
  record.header.kind = catalog::CatalogRecordKind::database;
  record.header.row_uuid = MakeUuid(UuidKind::row, 100);
  record.header.object_uuid = MakeUuid(UuidKind::object, 101);
  record.header.record_version = catalog::kCatalogRecordSchemaVersionCurrent;
  record.payload = "pcr136=upgrade_migration_gate";
  return record;
}

void CheckCatalogCodecAndPage(std::vector<EvidenceRow>* rows) {
  const auto record = MakeCatalogRecord();
  const auto encoded = catalog::EncodeCatalogTypedRecord(record, 1);
  Require(encoded.ok(), "PCR-136 current catalog record did not encode");
  AddRow(rows,
         "catalog_record_codec",
         "current_schema",
         "accepted",
         "",
         "current",
         "catalog_record_codec");

  auto future_record = record;
  future_record.header.record_version =
      catalog::kCatalogRecordSchemaVersionMaxSupported + 1;
  const auto future_record_result =
      catalog::EncodeCatalogTypedRecord(future_record, 2);
  Require(!future_record_result.ok() &&
              future_record_result.diagnostic.diagnostic_code ==
                  "SB-CATALOG-RECORD-CODEC-VERSION-UNSUPPORTED",
          "PCR-136 future catalog record version was accepted");
  AddRow(rows,
         "catalog_record_codec",
         "future_schema",
         "fail_closed",
         future_record_result.diagnostic.diagnostic_code,
         "newer-than-supported-refused",
         "catalog_record_codec");

  const page::CatalogPageRow row{
      page::CatalogPageRowKind::typed_catalog_record, 1, encoded.row.payload};
  const auto page_set = page::BuildCatalogPageSet({row}, 8192, 10, 20);
  Require(page_set.ok() && !page_set.pages.empty(),
          "PCR-136 current catalog page body did not build");
  const auto parsed =
      page::ParseCatalogPageBody(page_set.pages.front().body,
                                 page_set.pages.front().page_number);
  Require(parsed.ok(), "PCR-136 current catalog page body did not parse");
  AddRow(rows,
         "catalog_page_body",
         "current_format",
         "accepted",
         "",
         "current",
         "catalog_page_parser");

  auto future_minor = page_set.pages.front().body;
  Require(future_minor.size() > 11,
          "PCR-136 catalog page fixture shorter than format field");
  future_minor[10] =
      static_cast<platform::byte>(page::kCatalogPageBodyFormatMinorMaxSupported + 1);
  future_minor[11] = static_cast<platform::byte>(0);
  const auto future_page =
      page::ParseCatalogPageBody(future_minor,
                                 page_set.pages.front().page_number);
  Require(!future_page.ok() &&
              future_page.diagnostic.diagnostic_code ==
                  "SB-CATALOG-PAGE-BODY-FORMAT-UNSUPPORTED",
          "PCR-136 future catalog page format was accepted");
  AddRow(rows,
         "catalog_page_body",
         "future_minor",
         "fail_closed",
         future_page.diagnostic.diagnostic_code,
         "newer-than-supported-refused",
         "catalog_page_parser");
}

void CheckCatalogMigrationEvidence(std::vector<EvidenceRow>* rows) {
  db::DatabaseCatalogMigrationEvidence evidence;
  const auto current =
      db::ClassifyDatabaseCatalogMigrationEvidence(evidence);
  RequireArtifactOk(current,
                    db::DatabaseOpenCompatibilityClass::current,
                    false,
                    "PCR-136 current catalog migration evidence refused");
  AddRow(rows,
         "database_catalog_manifest",
         "current_evidence",
         "accepted",
         "",
         db::DatabaseOpenCompatibilityClassName(current.compatibility_class),
         "catalog_migration_evidence");

  evidence.database_catalog_manifest_format_version = 0;
  const auto missing_plan =
      db::ClassifyDatabaseCatalogMigrationEvidence(evidence);
  RequireArtifactRefusal(
      missing_plan,
      db::DatabaseOpenCompatibilityClass::migration_required_without_plan_refused,
      "ENGINE.DBLC_MIGRATION_REQUIRED_WITHOUT_PLAN",
      "PCR-136 catalog migration without plan was accepted");
  AddRow(rows,
         "database_catalog_manifest",
         "older_manifest_missing_plan",
         "fail_closed",
         missing_plan.diagnostic.diagnostic_code,
         db::DatabaseOpenCompatibilityClassName(missing_plan.compatibility_class),
         "catalog_migration_evidence");

  evidence.migration_plan_id =
      "database_catalog_manifest_v0_0_to_v1_0_explicit_plan_v1";
  const auto supported =
      db::ClassifyDatabaseCatalogMigrationEvidence(evidence);
  RequireArtifactOk(supported,
                    db::DatabaseOpenCompatibilityClass::supported_migration,
                    true,
                    "PCR-136 explicit catalog migration plan refused");
  AddRow(rows,
         "database_catalog_manifest",
         "older_manifest_explicit_plan",
         "supported_migration",
         "",
         db::DatabaseOpenCompatibilityClassName(supported.compatibility_class),
         "catalog_migration_evidence");

  evidence = db::DatabaseCatalogMigrationEvidence{};
  evidence.database_catalog_record_count = 2;
  const auto ambiguous =
      db::ClassifyDatabaseCatalogMigrationEvidence(evidence);
  RequireArtifactRefusal(ambiguous,
                         db::DatabaseOpenCompatibilityClass::ambiguous_identity_refused,
                         "ENGINE.DBLC_MIGRATION_AMBIGUOUS_IDENTITY_REFUSED",
                         "PCR-136 ambiguous catalog migration evidence accepted");
  AddRow(rows,
         "database_catalog_manifest",
         "ambiguous_identity",
         "fail_closed",
         ambiguous.diagnostic.diagnostic_code,
         db::DatabaseOpenCompatibilityClassName(ambiguous.compatibility_class),
         "catalog_migration_evidence");
}

void CheckClusterCatalogMigration(std::vector<EvidenceRow>* rows) {
  Require(!catalog::BuiltinClusterCatalogSchemaVersionProfiles().empty(),
          "PCR-136 cluster schema profile inventory empty");
  const auto& profile =
      catalog::BuiltinClusterCatalogSchemaVersionProfiles().front();

  catalog::ClusterCatalogCompatibilityRequest request;
  request.table_path = profile.table_path;
  request.schema_version = profile.schema_version_current;
  request.codec_version = profile.codec_version_current;
  request.external_provider_available = true;
  const auto current =
      catalog::EvaluateClusterCatalogCompatibility(request);
  Require(current.ok(), "PCR-136 current cluster catalog version refused");
  AddRow(rows,
         "cluster_catalog_schema",
         "current_schema",
         "accepted",
         "",
         catalog::ClusterCatalogCompatibilityClassName(current.compatibility_class),
         "external_provider_bound_cluster_catalog");

  request.downgrade_requested = true;
  const auto downgrade =
      catalog::EvaluateClusterCatalogCompatibility(request);
  Require(!downgrade.ok() &&
              downgrade.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-SCHEMA-DOWNGRADE-REFUSED",
          "PCR-136 cluster catalog downgrade was accepted");
  AddRow(rows,
         "cluster_catalog_schema",
         "downgrade_requested",
         "fail_closed",
         downgrade.diagnostic.diagnostic_code,
         catalog::ClusterCatalogCompatibilityClassName(downgrade.compatibility_class),
         "external_provider_bound_cluster_catalog");

  catalog::ClusterCatalogMigrationPlanRequest migration;
  migration.table_path = profile.table_path;
  migration.from_schema_version = 0;
  migration.to_schema_version = profile.schema_version_current;
  migration.external_provider_available = true;
  const auto missing_plan =
      catalog::ValidateClusterCatalogMigrationPlan(migration);
  Require(!missing_plan.ok() &&
              missing_plan.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-MIGRATION-PLAN-MISSING",
          "PCR-136 cluster catalog migration without plan accepted");
  AddRow(rows,
         "cluster_catalog_schema",
         "missing_migration_plan",
         "fail_closed",
         missing_plan.diagnostic.diagnostic_code,
         catalog::ClusterCatalogCompatibilityClassName(missing_plan.compatibility_class),
         "external_provider_bound_cluster_catalog");

  migration.migration_plan_id = "cluster_catalog_v0_to_v1";
  const auto unsupported =
      catalog::ValidateClusterCatalogMigrationPlan(migration);
  Require(!unsupported.ok() &&
              unsupported.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-MIGRATION-UNSUPPORTED",
          "PCR-136 unsupported cluster catalog migration plan accepted");
  AddRow(rows,
         "cluster_catalog_schema",
         "unsupported_migration_plan",
         "fail_closed",
         unsupported.diagnostic.diagnostic_code,
         catalog::ClusterCatalogCompatibilityClassName(unsupported.compatibility_class),
         "external_provider_bound_cluster_catalog");
}

index::IndexMetapageControl MetapageForDescriptor(
    const index::IndexFamilyDescriptor& descriptor) {
  index::IndexMetapageControl control;
  control.index_uuid = MakeUuid(UuidKind::object, 200);
  control.family = descriptor.family;
  control.root_page_number = 100;
  control.resource_epoch = 200;
  control.mutation_epoch = 300;
  control.root_generation = 400;
  control.page_count = 8;
  control.tuple_count_estimate = 64;
  control.layout_version = 1;
  control.semantic_profile_id = descriptor.default_semantic_profile;
  return control;
}

void CheckIndexMetapageCompatibility(std::vector<EvidenceRow>* rows) {
  const index::IndexFamilyDescriptor* descriptor = nullptr;
  for (const auto& candidate : index::BuiltinIndexFamilyDescriptors()) {
    if (candidate.persistence == index::IndexPersistenceClass::persistent &&
        candidate.completion ==
            index::IndexCompletionStatus::accepted_requires_full_implementation) {
      descriptor = &candidate;
      break;
    }
  }
  Require(descriptor != nullptr, "PCR-136 no persistent index descriptor found");

  const auto built = index::BuildIndexMetapageControl(
      MetapageForDescriptor(*descriptor));
  Require(built.ok(), "PCR-136 current index metapage did not build");
  const auto parsed = index::ParseIndexMetapageControl(built.serialized);
  Require(parsed.ok(), "PCR-136 current index metapage did not parse");
  AddRow(rows,
         "index_metapage",
         "current_durable_metadata",
         "accepted",
         "",
         "current",
         "index_durable_metadata_validator");

  auto future = parsed.control;
  future.metadata_format_version += 1;
  future.format_compatible = false;
  const auto future_result =
      index::ValidateIndexMetapageDurableMetadata(future);
  Require(!future_result.ok() &&
              future_result.diagnostic.diagnostic_code ==
                  "SB-INDEX-METAPAGE-DURABLE-METADATA-INVALID",
          "PCR-136 future index metadata was accepted");
  AddRow(rows,
         "index_metapage",
         "future_metadata_format",
         "fail_closed",
         future_result.diagnostic.diagnostic_code,
         "newer-than-supported-refused",
         "index_durable_metadata_validator");
}

void CheckDatatypeWireCompatibility(std::vector<EvidenceRow>* rows) {
  datatype::ParameterDataPacket packet;
  datatype::ParameterRowValueFrame frame;
  frame.row_ordinal = 0;
  packet.row_value_frames.push_back(frame);
  const auto current = datatype::ValidateParameterDataPacket(packet);
  Require(current.ok, "PCR-136 current datatype parameter packet refused");
  AddRow(rows,
         "datatype_wire_metadata",
         "current_parameter_packet",
         "accepted",
         "",
         "current",
         "native_wire_metadata");

  packet.layout_version =
      datatype::kNativeWireMetadataLayoutVersion + 1;
  const auto future = datatype::ValidateParameterDataPacket(packet);
  Require(!future.ok &&
              future.diagnostic_code == "NATIVE_WIRE.LAYOUT_VERSION_UNSUPPORTED",
          "PCR-136 future datatype wire layout accepted");
  AddRow(rows,
         "datatype_wire_metadata",
         "future_parameter_packet_layout",
         "fail_closed",
         future.diagnostic_code,
         "newer-than-supported-refused",
         "native_wire_metadata");

  datatype::RowDescriptionPacket row_description;
  const auto current_row =
      datatype::ValidateRowDescriptionPacket(row_description);
  Require(current_row.ok, "PCR-136 current row description refused");
  row_description.layout_version =
      datatype::kNativeWireMetadataLayoutVersion + 1;
  const auto future_row =
      datatype::ValidateRowDescriptionPacket(row_description);
  Require(!future_row.ok &&
              future_row.diagnostic_code ==
                  "NATIVE_WIRE.LAYOUT_VERSION_UNSUPPORTED",
          "PCR-136 future row description layout accepted");
  AddRow(rows,
         "datatype_wire_metadata",
         "future_row_description_layout",
         "fail_closed",
         future_row.diagnostic_code,
         "newer-than-supported-refused",
         "native_wire_metadata");
}

void CheckStartupRollbackEvidence(std::vector<EvidenceRow>* rows) {
  const auto current =
      db::ClassifyStartupStateFormatCompatibility(
          db::kStartupStateFormatMajorCurrent,
          db::kStartupStateFormatMinorCurrent);
  Require(current.ok(), "PCR-136 current startup state format refused");
  AddRow(rows,
         "startup_state",
         "current_format",
         "accepted",
         "",
         db::StartupStateFormatCompatibilityClassName(
             current.compatibility_class),
         "startup_state_format_classifier");

  const auto missing_plan =
      db::ClassifyStartupStateFormatCompatibility(
          db::kStartupStateFormatMajorCurrent,
          db::kStartupStateFormatMinorCurrent,
          {},
          false,
          true);
  Require(!missing_plan.ok() &&
              missing_plan.diagnostic.diagnostic_code ==
                  "SB-STARTUP-STATE-MIGRATION-PLAN-MISSING",
          "PCR-136 startup migration without plan accepted");
  AddRow(rows,
         "startup_state",
         "migration_plan_required",
         "fail_closed",
         missing_plan.diagnostic.diagnostic_code,
         db::StartupStateFormatCompatibilityClassName(
             missing_plan.compatibility_class),
         "startup_state_format_classifier");

  const auto downgrade =
      db::ClassifyStartupStateFormatCompatibility(
          db::kStartupStateFormatMajorCurrent,
          db::kStartupStateFormatMinorCurrent,
          {},
          true,
          false);
  Require(!downgrade.ok() &&
              downgrade.diagnostic.diagnostic_code ==
                  "SB-STARTUP-STATE-FORMAT-DOWNGRADE-REFUSED",
          "PCR-136 startup downgrade accepted");
  AddRow(rows,
         "startup_state",
         "downgrade_requested",
         "fail_closed",
         downgrade.diagnostic.diagnostic_code,
         db::StartupStateFormatCompatibilityClassName(
             downgrade.compatibility_class),
         "startup_state_format_classifier");

  auto state = db::MakeInitialStartupState(MakeUuid(UuidKind::database, 300),
                                           MakeUuid(UuidKind::filespace, 301),
                                           16384);
  state = db::MarkStartupDirty(
      std::move(state),
      "owner:pcr136_interrupted_upgrade",
      db::StartupRecoveryClassification::fence_writes_until_safe);
  Require(state.startup_dirty && state.write_admission_fenced &&
              !state.owner_token.empty(),
          "PCR-136 interrupted startup state was not write-fenced");
  AddRow(rows,
         "startup_state",
         "interrupted_upgrade_recovery",
         "write_fenced",
         "",
         db::StartupRecoveryClassificationName(
             state.recovery_classification),
         "startup_recovery_classifier");

  state = db::RecordStartupLifecycleEvidence(
      std::move(state),
      db::StartupLifecycleDurablePhase::restricted_open_entered,
      77,
      1783600000000ull,
      db::StartupLifecycleEvidenceFlag::restricted_open_evidence_recorded);
  Require(db::StartupLifecycleEvidencePresent(
              state,
              db::StartupLifecycleEvidenceFlag::restricted_open_evidence_recorded),
          "PCR-136 restricted-open evidence flag missing");
  AddRow(rows,
         "startup_state",
         "rollback_before_commit",
         "restricted_open_evidence_recorded",
         "",
         db::StartupLifecycleDurablePhaseName(
             state.durable_lifecycle_phase),
         "startup_recovery_classifier");
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path csv_output;
  if (argc == 2) {
    csv_output = argv[1];
  } else if (argc != 1) {
    std::cerr << "usage: public_upgrade_migration_gate [evidence.csv]\n";
    return EXIT_FAILURE;
  }

  std::vector<EvidenceRow> rows;
  CheckVersionedArtifactSurface("odf_header", "odf_header", &rows);
  CheckVersionedArtifactSurface("policy_pack_manifest",
                                "policy_pack_manifest",
                                &rows);
  CheckCatalogCodecAndPage(&rows);
  CheckCatalogMigrationEvidence(&rows);
  CheckClusterCatalogMigration(&rows);
  CheckIndexMetapageCompatibility(&rows);
  CheckDatatypeWireCompatibility(&rows);
  CheckStartupRollbackEvidence(&rows);

  std::size_t fail_closed = 0;
  std::size_t supported_migrations = 0;
  for (const auto& row : rows) {
    if (row.decision == "fail_closed") {
      ++fail_closed;
    }
    if (row.decision == "supported_migration") {
      ++supported_migrations;
    }
  }
  Require(rows.size() >= 30,
          "PCR-136 upgrade migration evidence matrix too small");
  Require(fail_closed >= 15,
          "PCR-136 fail-closed compatibility coverage too small");
  Require(supported_migrations >= 3,
          "PCR-136 supported migration proof coverage too small");

  WriteEvidenceCsv(csv_output, rows);
  std::cout << "public_upgrade_migration_gate=passed rows=" << rows.size()
            << " fail_closed=" << fail_closed
            << " supported_migrations=" << supported_migrations << '\n';
  return EXIT_SUCCESS;
}
