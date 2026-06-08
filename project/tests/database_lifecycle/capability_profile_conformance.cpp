// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_feature_gates.hpp"
#include "config.hpp"
#include "config_policy_security_lifecycle.hpp"
#include "parser_package_registry.hpp"
#include "sbps.hpp"
#include "sb_udr_runtime.hpp"
#include "sblr_admission.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#ifndef SB_SPEC_MANIFEST
#define SB_SPEC_MANIFEST ""
#endif

namespace {

namespace agents = scratchbird::core::agents;
namespace server = scratchbird::server;
namespace sbps = scratchbird::server::sbps;
namespace udr_runtime = scratchbird::udr::runtime;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc013aj_capability.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-013AJ capability test");
  return std::filesystem::path(made);
}

bool HasDiagnostic(const std::vector<server::ServerDiagnostic>& diagnostics,
                   std::string_view code) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

udr_runtime::UdrCallResult ParserSupportProbe(
    const udr_runtime::UdrCallInput& input) {
  udr_runtime::UdrCallResult result;
  result.ok = !input.package_uuid.empty();
  result.payload = "DBLC-013AJ-parser-support-probe";
  result.message_vector_json = "{\"diagnostic\":\"UDR.OK\"}";
  return result;
}

sbps::HelloRequest BuiltinHello() {
  const auto payload = sbps::EncodeHelloRequestForTest();
  auto hello = sbps::DecodeHelloRequest(payload);
  Require(hello.has_value(), "test HELLO payload did not decode");
  return *hello;
}

agents::InstalledCapabilityRecord InstalledCapability() {
  agents::InstalledCapabilityRecord record;
  record.capability_id = "parser.sbsql.builtin";
  record.provider_id = "sb_server";
  record.edition_scope = agents::CapabilityEditionScope::enterprise;
  record.lifecycle_state = agents::CapabilityLifecycleState::enabled;
  record.capability_epoch = 3;
  record.installed_policy_epoch = 5;
  record.requires_parser_package = true;
  record.required_parser_package_id = "builtin_test_package";
  record.parser_package_installed = true;
  return record;
}

agents::FeatureGateRequest GateRequest() {
  agents::FeatureGateRequest request;
  request.request_id = "dblc013aj";
  request.capability_id = "parser.sbsql.builtin";
  request.requested_edition_scope = agents::CapabilityEditionScope::community;
  request.observed_policy_epoch = 5;
  request.minimum_capability_epoch = 2;
  request.parser_package_available = true;
  return request;
}

server::ParserPackageRegistry RegistryWith(server::ParserPackageRegistryEntry entry,
                                           std::uint64_t capability_policy_generation = 5) {
  server::ParserPackageRegistry registry;
  registry.generation = 13;
  registry.capability_policy_generation = capability_policy_generation;
  registry.source = "database_lifecycle_capability_profile";
  registry.entries.push_back(std::move(entry));
  return registry;
}

void TestCoreFeatureGateModel() {
  const auto record = InstalledCapability();
  const auto request = GateRequest();
  const auto allowed = agents::EvaluateFeatureGateRequest(record, request, 5);
  Require(allowed.decision_class == agents::FeatureGateDecisionClass::allow,
          "installed enabled capability was not allowed");

  auto unknown = request;
  unknown.capability_id = "unknown.capability";
  const auto unknown_result =
      agents::EvaluateFeatureGateRequest(std::vector<agents::InstalledCapabilityRecord>{record},
                                         unknown,
                                         5);
  Require(unknown_result.decision_class == agents::FeatureGateDecisionClass::fail_closed &&
              unknown_result.diagnostic_code == "SB_AGENT_CAPABILITY.NOT_INSTALLED",
          "unknown capability did not fail closed");

  auto stale = request;
  stale.observed_policy_epoch = 4;
  const auto stale_result = agents::EvaluateFeatureGateRequest(record, stale, 5);
  Require(stale_result.diagnostic_code == "SB_AGENT_CAPABILITY.POLICY_EPOCH_STALE",
          "stale capability policy epoch was not refused");

  auto enterprise_request = request;
  enterprise_request.requested_edition_scope = agents::CapabilityEditionScope::cluster;
  const auto edition_result =
      agents::EvaluateFeatureGateRequest(record, enterprise_request, 5);
  Require(edition_result.diagnostic_code == "SB_AGENT_CAPABILITY.EDITION_SCOPE_DENIED",
          "edition gate refusal was not enforced");

  auto missing_parser = record;
  missing_parser.parser_package_installed = false;
  auto parser_request = request;
  parser_request.parser_package_available = false;
  const auto parser_result =
      agents::EvaluateFeatureGateRequest(missing_parser, parser_request, 5);
  Require(parser_result.diagnostic_code == "SB_AGENT_CAPABILITY.PARSER_PACKAGE_REQUIRED",
          "parser package requirement did not fail closed");

  auto cluster_record = record;
  cluster_record.cluster_authority_required = true;
  const auto cluster_result =
      agents::EvaluateFeatureGateRequest(cluster_record, request, 5);
  Require(cluster_result.diagnostic_code == "SB_AGENT_CAPABILITY.CLUSTER_AUTHORITY_REQUIRED",
          "cluster-only capability admitted without cluster authority");

  auto downgrade = record;
  downgrade.capability_epoch = 2;
  const auto downgrade_status =
      agents::ValidateCapabilityNoDowngrade(record, downgrade);
  Require(!downgrade_status.ok &&
              downgrade_status.diagnostic_code == "SB_AGENT_CAPABILITY.DOWNGRADE_REFUSED",
          "capability downgrade was not refused");
}

void TestParserPackageCapabilityAdmission() {
  constexpr std::string_view kSupportUdrUuid =
      "019e13aj-0000-7000-8000-0000000000aa";
  udr_runtime::ResetRuntimeForTest();
  udr_runtime::UdrPackageDescriptor descriptor;
  descriptor.package_uuid = std::string(kSupportUdrUuid);
  descriptor.package_name = "builtin test parser support UDR";
  descriptor.abi_version = "sb_udr_v1";
  descriptor.source_revision = "capability-profile-conformance";
  descriptor.binary_hash = "capability-profile-test-hash";
  descriptor.signature_policy = "trusted_test_descriptor";
  descriptor.capability_role = "parser_support";
  descriptor.trusted_cpp = true;
  descriptor.entrypoints.push_back({"support_probe", "parser_support", ParserSupportProbe});
  Require(udr_runtime::RegisterPackage(descriptor).ok,
          "parser-support UDR test descriptor did not register");
  Require(udr_runtime::LoadPackage(kSupportUdrUuid).ok,
          "parser-support UDR test descriptor did not load");

  auto entry = server::ParserPackageRegistryEntry{};
  entry.required_capability_id = "parser.sbsql.builtin";
  entry.capability_policy_generation = 5;
  entry.capability_epoch = 2;
  entry.capability_installed = true;
  entry.capability_enabled = true;
  entry.parser_support_udr_required = true;
  entry.parser_support_udr_available = true;
  entry.parser_support_udr_uuid = std::string(kSupportUdrUuid);
  entry.parser_support_udr_source_revision = descriptor.source_revision;
  entry.parser_support_udr_binary_hash = descriptor.binary_hash;
  entry.parser_support_udr_signature_policy = descriptor.signature_policy;
  entry.parser_support_udr_capability_role = descriptor.capability_role;
  const auto admitted =
      server::AdmitParserPackage(RegistryWith(entry), BuiltinHello(), sbps::kProtocolMajor, 0);
  Require(admitted.admitted, "capability-admitted parser package was rejected");

  auto disabled = entry;
  disabled.capability_enabled = false;
  const auto disabled_result =
      server::AdmitParserPackage(RegistryWith(disabled), BuiltinHello(), sbps::kProtocolMajor, 0);
  Require(!disabled_result.admitted &&
              HasDiagnostic(disabled_result.diagnostics, "SB_AGENT_CAPABILITY.NOT_ACTIVE"),
          "disabled capability did not reject parser package");

  auto stale = entry;
  stale.capability_policy_generation = 4;
  const auto stale_result =
      server::AdmitParserPackage(RegistryWith(stale, 5), BuiltinHello(), sbps::kProtocolMajor, 0);
  Require(!stale_result.admitted &&
              HasDiagnostic(stale_result.diagnostics,
                            "SB_AGENT_CAPABILITY.POLICY_EPOCH_REVALIDATION_REQUIRED"),
          "stale parser capability policy did not fail closed");

  auto cluster = entry;
  cluster.cluster_capability_required = true;
  const auto cluster_result =
      server::AdmitParserPackage(RegistryWith(cluster), BuiltinHello(), sbps::kProtocolMajor, 0);
  Require(!cluster_result.admitted &&
              HasDiagnostic(cluster_result.diagnostics,
                            "SB_AGENT_CAPABILITY.CLUSTER_AUTHORITY_REQUIRED"),
          "cluster parser capability admitted without authority");
}

void TestConfigCapabilityPolicyLifecycle(const std::filesystem::path& temp_dir) {
  const auto config_path = temp_dir / "sb_server.conf";
  std::ofstream out(config_path, std::ios::trunc);
  out << "[config]\n";
  out << "format = SBCD1\n";
  out << "[server.config]\n";
  out << "source_epoch = 2\n";
  out << "reload_generation = 2\n";
  out << "[server.capability]\n";
  out << "policy_generation = 7\n";
  out << "[server.security]\n";
  out << "policy_generation = 3\n";
  out << "epoch = 4\n";
  out << "provider_generation = 5\n";
  Require(static_cast<bool>(out), "capability config write failed");
  out.close();

  server::ServerCliOptions cli;
  cli.config_path = config_path.string();
  const auto resolved = server::ResolveServerBootstrapConfig(cli);
  Require(resolved.ok(), "capability policy config did not parse");
  Require(resolved.config.capability_policy_generation == 7,
          "capability policy generation did not parse");
  Require(resolved.config.cache_invalidation_epoch == 7,
          "capability policy generation did not raise cache epoch");

  server::ServerBootstrapConfig config;
  config.capability_policy_generation = 7;
  config.security_policy_generation = 3;
  config.security_epoch = 4;
  config.security_provider_generation = 5;
  config.cache_invalidation_epoch = 7;
  const auto start = server::StartConfigPolicySecurityLifecycle(
      server::BuildConfigPolicySecurityLifecycleInput(config,
                                                      (temp_dir / "example.sbdb").string(),
                                                      "019e13a9-0000-7000-8000-000000000001",
                                                      true,
                                                      false));
  Require(start.ok(), "capability policy lifecycle did not start");
  Require(start.lifecycle.capability_policy_generation == 7,
          "capability policy lifecycle generation mismatch");
  Require(server::SerializeConfigPolicySecurityLifecycleJson(start.lifecycle)
              .find("capability_policy_cache") != std::string::npos,
          "capability policy cache invalidation target missing");

  auto stale = server::ValidateConfigPolicySecurityAdmission(
      start.lifecycle, 6, 3, 4, 5, "local_password", "engine");
  Require(!stale.ok() && stale.diagnostic.code == "ENGINE.DBLC_STALE_POLICY_REFUSED",
          "stale capability policy admission was not refused");
}

void TestSblrAndManifestBoundary() {
  server::ServerSblrAdmissionRequest cluster;
  cluster.encoded_sblr_envelope =
      "operation_id=cluster.route\n"
      "result_shape=rows\n"
      "diagnostic_shape=message_vector\n"
      "parser_resolved_names_to_uuids=true\n"
      "requires_cluster_authority=true\n";
  const auto cluster_result = server::AdmitServerSblrEnvelope(cluster);
  Require(!cluster_result.admitted &&
              HasDiagnostic(cluster_result.diagnostics, "SBLR.CAPABILITY.FORBIDDEN"),
          "cluster-only SBLR did not fail closed");

  server::ServerSblrAdmissionRequest raw_sql;
  raw_sql.encoded_sblr_envelope = "select * from users.public.example";
  const auto raw_sql_result = server::AdmitServerSblrEnvelope(raw_sql);
  Require(!raw_sql_result.admitted &&
              HasDiagnostic(raw_sql_result.diagnostics, "SBLR.SQL_TEXT_FORBIDDEN"),
          "raw SQL text was not refused before server SBLR admission");

  std::ifstream manifest(SB_SPEC_MANIFEST);
  Require(static_cast<bool>(manifest), "contract manifest was not available to DBLC-013AJ test");
  std::string manifest_text;
  std::string line;
  while (std::getline(manifest, line)) {
    manifest_text += line;
    manifest_text.push_back('\n');
  }
  Require(Contains(manifest_text, "registries/donor-parser-feature-gates.yaml"),
          "donor parser feature-gate registry is not manifest-authoritative");
  Require(Contains(manifest_text, "registries/builtin-library-feature-gates.yaml"),
          "built-in library feature-gate registry is not manifest-authoritative");
}

}  // namespace

int main() {
  const auto temp_dir = MakeTempDir();
  TestCoreFeatureGateModel();
  TestParserPackageCapabilityAdmission();
  TestConfigCapabilityPolicyLifecycle(temp_dir);
  TestSblrAndManifestBoundary();
  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}
