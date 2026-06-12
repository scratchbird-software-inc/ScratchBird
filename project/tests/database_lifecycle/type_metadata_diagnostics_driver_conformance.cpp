// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/value.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace engine = scratchbird::engine;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

engine::Uuid Uuid(std::uint8_t seed) {
  engine::Uuid uuid;
  for (std::size_t index = 0; index < 16; ++index) {
    uuid.bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return uuid;
}

engine::ExecutionTypeDescriptor TypeDescriptor(std::uint8_t seed,
                                               std::string_view name) {
  engine::ExecutionTypeDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid(seed);
  descriptor.descriptor_epoch = 101;
  descriptor.canonical_type_id = seed;
  descriptor.family = engine::ExecutionTypeFamily::signed_integer;
  descriptor.width_class = engine::ExecutionTypeWidthClass::fixed;
  descriptor.stable_name = std::string(name);
  descriptor.bit_width = 32;
  return descriptor;
}

engine::DomainCppUdrOperationHook ValidHook() {
  engine::DomainCppUdrOperationHook hook;
  hook.present = true;
  hook.library_uuid = Uuid(0xe0);
  hook.mapping_descriptor_uuid = Uuid(0xe1);
  hook.mapping_descriptor_epoch = 101;
  hook.entrypoint_symbol = "sb_tmd_bridge";
  hook.abi_major = 1;
  hook.preserves_descriptors = true;
  hook.parser_independent = true;
  return hook;
}

engine::TypeMetadataDiagnosticPayload Diagnostic(
    const engine::Uuid& descriptor_uuid,
    engine::TypeMetadataDiagnosticPhase phase,
    std::string_view code) {
  engine::TypeMetadataDiagnosticPayload diagnostic;
  diagnostic.diagnostic_uuid = Uuid(0x50);
  diagnostic.scratchbird_code = std::string(code);
  diagnostic.severity = "error";
  diagnostic.descriptor_uuid = descriptor_uuid;
  diagnostic.driver_family = engine::DriverMetadataFamily::native;
  diagnostic.compatibility_class =
      engine::TypeMetadataCompatibilityClass::native_or_better;
  diagnostic.source_phase = phase;
  diagnostic.stable_search_key = std::string(code);
  return diagnostic;
}

engine::DriverTypeMetadataDescriptor DriverRow(
    engine::DriverMetadataFamily family,
    const engine::ExecutionTypeDescriptor& descriptor,
    std::uint8_t seed) {
  engine::DriverTypeMetadataDescriptor row;
  row.driver_metadata_uuid = Uuid(seed);
  row.descriptor_uuid = descriptor.descriptor_uuid;
  row.descriptor_epoch = descriptor.descriptor_epoch;
  row.descriptor = descriptor;
  row.driver_family = family;
  row.type_name = "INTEGER";
  row.native_type_code = "i32";
  row.sql_type_code = "SQL_INTEGER";
  row.precision = 10;
  row.scale = 0;
  row.display_size = 11;
  row.nullable = engine::DriverMetadataNullable::nullable;
  row.signedness = engine::DriverMetadataSignedness::signed_numeric;
  row.case_sensitive =
      engine::DriverMetadataCaseSensitivity::unknown_by_policy;
  row.searchable = engine::DriverMetadataSearchability::range;
  row.literal_prefix = "";
  row.literal_suffix = "";
  row.create_params = {"precision"};
  row.compatibility_class =
      engine::TypeMetadataCompatibilityClass::native_or_better;
  row.exposure_class = engine::TypeMetadataExposureClass::driver_rendered;
  row.diagnostic_policy_ref = Uuid(seed + 1);
  row.definition_hash = "driver.metadata.definition";
  if (family == engine::DriverMetadataFamily::donor_specific) {
    row.donor_family = "sqlserver";
    row.donor_version_profile = "2022";
    row.type_name = "int";
    row.exposure_class = engine::TypeMetadataExposureClass::donor_rendered;
  }
  return row;
}

engine::BackupRestoreTypeProfile BackupProfile(
    const engine::Uuid& descriptor_uuid) {
  engine::BackupRestoreTypeProfile profile;
  profile.profile_uuid = Uuid(0x60);
  profile.descriptor_uuid = descriptor_uuid;
  profile.canonical_encoding_uuid = Uuid(0x61);
  profile.physical_encoding_uuid = Uuid(0x62);
  profile.logical_rendering_uuid = Uuid(0x63);
  profile.descriptor_snapshot_uuid = Uuid(0x64);
  profile.resource_epoch = 102;
  profile.resource_profile_uuid = Uuid(0x65);
  profile.diagnostic =
      Diagnostic(descriptor_uuid,
                 engine::TypeMetadataDiagnosticPhase::backup_restore,
                 "TYPE_METADATA.BACKUP_PROFILE_OK");
  profile.conformance_key = "TMD-GATE-004";
  return profile;
}

engine::ReplicationTypeProfile ReplicationProfile(
    const engine::Uuid& descriptor_uuid) {
  engine::ReplicationTypeProfile profile;
  profile.profile_uuid = Uuid(0x70);
  profile.descriptor_uuid = descriptor_uuid;
  profile.delta_encoding_uuid = Uuid(0x71);
  profile.full_value_encoding_uuid = Uuid(0x72);
  profile.resource_epoch = 102;
  profile.resource_profile_uuid = Uuid(0x73);
  profile.diagnostic =
      Diagnostic(descriptor_uuid,
                 engine::TypeMetadataDiagnosticPhase::replication,
                 "TYPE_METADATA.REPLICATION_PROFILE_OK");
  profile.conformance_key = "TMD-GATE-005";
  return profile;
}

engine::ClusterTypeTransportProfile ClusterProfile(
    const engine::Uuid& descriptor_uuid) {
  engine::ClusterTypeTransportProfile profile;
  profile.profile_uuid = Uuid(0x80);
  profile.descriptor_uuid = descriptor_uuid;
  profile.transport_encoding_uuid = Uuid(0x81);
  profile.resource_profile_uuid = Uuid(0x82);
  profile.resource_epoch = 102;
  profile.diagnostic =
      Diagnostic(descriptor_uuid,
                 engine::TypeMetadataDiagnosticPhase::cluster_transport,
                 "TYPE_METADATA.CLUSTER_TRANSPORT_PROFILE_OK");
  profile.conformance_key = "TMD-GATE-006";
  return profile;
}

engine::UnsupportedDegradedTypeContract UnsupportedContract(
    const engine::Uuid& descriptor_uuid) {
  engine::UnsupportedDegradedTypeContract contract;
  contract.contract_uuid = Uuid(0x90);
  contract.descriptor_uuid = descriptor_uuid;
  contract.compatibility_class =
      engine::TypeMetadataCompatibilityClass::bridge_only;
  contract.diagnostic_code = "TYPE_METADATA.BRIDGE_UNAVAILABLE";
  contract.metadata_status = "bridge_only";
  contract.unsupported_reason = "requires trusted C++ bridge";
  contract.bridge_hook = ValidHook();
  return contract;
}

engine::TypeTestingCorpusDescriptor TestingCorpus(
    const engine::Uuid& descriptor_uuid) {
  engine::TypeTestingCorpusDescriptor corpus;
  corpus.corpus_uuid = Uuid(0xa0);
  corpus.descriptor_uuid = descriptor_uuid;
  corpus.donor_mapping_uuid = Uuid(0xa1);
  corpus.conformance_manifest_hash = "tmd.corpus.manifest";
  corpus.implemented_donor_mapping = true;
  return corpus;
}

std::vector<std::string> RequiredMetricNames() {
  return {"sys.metrics.type_metadata.catalog_query_count",
          "sys.metrics.type_metadata.driver_metadata_query_count",
          "sys.metrics.type_metadata.donor_metadata_query_count",
          "sys.metrics.type_metadata.hidden_row_count",
          "sys.metrics.type_metadata.redacted_field_count",
          "sys.metrics.type_metadata.unsupported_type_count",
          "sys.metrics.type_metadata.deferred_type_count",
          "sys.metrics.type_metadata.bridge_only_type_count",
          "sys.metrics.type_metadata.degraded_type_count",
          "sys.metrics.type_metadata.diagnostic_render_count",
          "sys.metrics.type_metadata.diagnostic_redaction_count",
          "sys.metrics.type_metadata.cache_hit_count",
          "sys.metrics.type_metadata.cache_stale_rejection_count",
          "sys.metrics.type_metadata.profile_missing_count"};
}

engine::TypeMetadataDiagnosticsDriverContract ValidContract() {
  const auto descriptor = TypeDescriptor(0x10, "tmd.i32");
  engine::TypeMetadataDiagnosticsDriverContract contract;
  contract.contract_uuid = Uuid(0x01);
  contract.contract_epoch = 101;
  contract.stable_name = "tmd.contract";
  contract.catalog_exposure.exposure_model_uuid = Uuid(0x02);
  contract.catalog_exposure.catalog_snapshot_uuid = Uuid(0x03);
  contract.catalog_exposure.schema_epoch = 101;
  contract.catalog_exposure.security_epoch = 102;
  contract.catalog_exposure.resource_epoch = 103;
  contract.driver_metadata_rows.push_back(
      DriverRow(engine::DriverMetadataFamily::odbc, descriptor, 0x20));
  contract.driver_metadata_rows.push_back(
      DriverRow(engine::DriverMetadataFamily::jdbc, descriptor, 0x24));
  contract.driver_metadata_rows.push_back(
      DriverRow(engine::DriverMetadataFamily::dotnet, descriptor, 0x28));
  contract.driver_metadata_rows.push_back(
      DriverRow(engine::DriverMetadataFamily::native, descriptor, 0x2c));
  contract.driver_metadata_rows.push_back(
      DriverRow(engine::DriverMetadataFamily::donor_specific, descriptor,
                0x30));
  contract.diagnostic_payloads.push_back(
      Diagnostic(descriptor.descriptor_uuid,
                 engine::TypeMetadataDiagnosticPhase::metadata_exposure,
                 "TYPE_METADATA.DRIVER_ROW_INVALID"));
  contract.backup_restore_profiles.push_back(
      BackupProfile(descriptor.descriptor_uuid));
  contract.replication_profiles.push_back(
      ReplicationProfile(descriptor.descriptor_uuid));
  contract.cluster_transport_profiles.push_back(
      ClusterProfile(descriptor.descriptor_uuid));
  contract.unsupported_contracts.push_back(
      UnsupportedContract(descriptor.descriptor_uuid));
  contract.testing_corpus.push_back(TestingCorpus(descriptor.descriptor_uuid));
  contract.cache_key.database_uuid = Uuid(0xb0);
  contract.cache_key.descriptor_uuid = descriptor.descriptor_uuid;
  contract.cache_key.driver_family = engine::DriverMetadataFamily::native;
  contract.cache_key.schema_epoch = 101;
  contract.cache_key.security_epoch = 102;
  contract.cache_key.resource_epoch = 103;
  contract.cache_key.capability_profile_definition_hash = "tmd.cache.hash";
  contract.cache_key.policy_uuid = Uuid(0xb1);
  contract.local_metric_names = RequiredMetricNames();
  return contract;
}

void RequireStatus(
    const engine::TypeMetadataDiagnosticsDriverContract& contract,
    engine::TypeMetadataDiagnosticsStatus expected,
    std::string_view message) {
  const auto result =
      engine::ValidateTypeMetadataDiagnosticsDriverContract(contract);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "TMD metadata diagnostics driver status mismatch");
}

void TestValidContractCoversTmdGates() {
  const auto contract = ValidContract();
  Require(engine::ValidateTypeMetadataDiagnosticsDriverContract(contract).ok(),
          "TMD rejected valid metadata diagnostics driver contract");
  Require(engine::TypeMetadataDiagnosticsStatusName(
              engine::TypeMetadataDiagnosticsStatus::ok) == "ok",
          "TMD status names are not stable");
}

void TestCatalogAndDriverFailures() {
  auto contract = ValidContract();
  contract.catalog_exposure.driver_metadata_exposed = false;
  RequireStatus(contract,
                engine::TypeMetadataDiagnosticsStatus::catalog_family_missing,
                "TMD accepted catalog exposure missing driver metadata");

  contract = ValidContract();
  contract.catalog_exposure.donor_views_do_not_create_authority = false;
  RequireStatus(contract,
                engine::TypeMetadataDiagnosticsStatus::
                    catalog_false_authority_guard_missing,
                "TMD accepted donor catalog false authority");

  contract = ValidContract();
  contract.driver_metadata_rows.pop_back();
  RequireStatus(contract,
                engine::TypeMetadataDiagnosticsStatus::
                    driver_family_coverage_missing,
                "TMD accepted missing donor-specific driver metadata");

  contract = ValidContract();
  contract.driver_metadata_rows[4].donor_family.clear();
  RequireStatus(contract,
                engine::TypeMetadataDiagnosticsStatus::
                    driver_donor_family_required,
                "TMD accepted donor driver row without donor identity");

  contract = ValidContract();
  contract.driver_metadata_rows[0].donor_name_not_authority = false;
  RequireStatus(contract,
                engine::TypeMetadataDiagnosticsStatus::
                    driver_false_authority_guard_missing,
                "TMD accepted driver row using donor name as authority");
}

void TestDiagnosticProfileAndUnsupportedFailures() {
  auto contract = ValidContract();
  contract.diagnostic_payloads[0].protected_values_redacted = false;
  RequireStatus(contract,
                engine::TypeMetadataDiagnosticsStatus::
                    diagnostic_redaction_failed,
                "TMD accepted diagnostic leaking protected values");

  contract = ValidContract();
  contract.backup_restore_profiles.clear();
  RequireStatus(contract,
                engine::TypeMetadataDiagnosticsStatus::backup_profile_required,
                "TMD accepted missing backup/restore profile");

  contract = ValidContract();
  contract.backup_restore_profiles[0].logical_backup_supported = false;
  RequireStatus(contract,
                engine::TypeMetadataDiagnosticsStatus::
                    backup_profile_policy_missing,
                "TMD accepted incomplete backup profile policy");

  contract = ValidContract();
  contract.replication_profiles[0].fails_closed_when_unproven = false;
  RequireStatus(contract,
                engine::TypeMetadataDiagnosticsStatus::
                    replication_profile_policy_missing,
                "TMD accepted replication profile without fail-closed policy");

  contract = ValidContract();
  contract.cluster_transport_profiles[0].rejects_incompatible_resources =
      false;
  RequireStatus(contract,
                engine::TypeMetadataDiagnosticsStatus::
                    cluster_profile_policy_missing,
                "TMD accepted cluster transport profile without refusal policy");

  contract = ValidContract();
  contract.cluster_transport_profiles[0].cluster_authority_recorded = true;
  RequireStatus(contract,
                engine::TypeMetadataDiagnosticsStatus::
                    cluster_authority_without_governance,
                "TMD accepted cluster authority without governance");

  contract = ValidContract();
  contract.unsupported_contracts[0].silent_fallback_impossible = false;
  RequireStatus(contract,
                engine::TypeMetadataDiagnosticsStatus::
                    unsupported_contract_silent_fallback,
                "TMD accepted unsupported contract allowing silent fallback");

  contract = ValidContract();
  contract.unsupported_contracts[0].bridge_hook.present = false;
  RequireStatus(contract,
                engine::TypeMetadataDiagnosticsStatus::
                    unsupported_contract_bridge_invalid,
                "TMD accepted bridge-only type without C++ bridge proof");
}

void TestCorpusCacheAndMetricFailures() {
  auto contract = ValidContract();
  contract.testing_corpus[0].cast_case = false;
  RequireStatus(contract,
                engine::TypeMetadataDiagnosticsStatus::
                    testing_corpus_case_missing,
                "TMD accepted donor mapping corpus missing cast case");

  contract = ValidContract();
  contract.cache_key.parser_family_untrusted_context_only = false;
  RequireStatus(contract,
                engine::TypeMetadataDiagnosticsStatus::cache_key_parser_authority,
                "TMD accepted parser family as metadata authority");

  contract = ValidContract();
  contract.cache_key.metadata_query_loads_udr = true;
  RequireStatus(contract,
                engine::TypeMetadataDiagnosticsStatus::metadata_query_side_effect,
                "TMD accepted metadata query with UDR execution side effect");

  contract = ValidContract();
  contract.local_metric_names.pop_back();
  RequireStatus(contract,
                engine::TypeMetadataDiagnosticsStatus::local_metric_missing,
                "TMD accepted missing type metadata metric");

  contract = ValidContract();
  contract.cluster_metrics_guarded_by_cluster_governance = false;
  RequireStatus(contract,
                engine::TypeMetadataDiagnosticsStatus::cluster_metrics_guard_required,
                "TMD accepted cluster metadata metrics without governance guard");
}

}  // namespace

int main() {
  TestValidContractCoversTmdGates();
  TestCatalogAndDriverFailures();
  TestDiagnosticProfileAndUnsupportedFailures();
  TestCorpusCacheAndMetricFailures();
  std::cout << "type_metadata_diagnostics_driver_conformance=passed\n";
  return EXIT_SUCCESS;
}
