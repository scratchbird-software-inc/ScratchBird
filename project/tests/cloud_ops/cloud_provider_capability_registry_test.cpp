// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cloud/cloud_deployment_profile.hpp"
#include "cloud/cloud_provider_capability.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool HasDiagnostic(const std::vector<api::EngineApiDiagnostic>& diagnostics,
                   std::string_view code) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

bool RowHasField(const api::CloudProviderCapabilityProjectionRow& row,
                 std::string_view field_name) {
  for (const auto& field : row.fields) {
    if (field.first == field_name) {
      return true;
    }
  }
  return false;
}

api::CloudProviderCapabilityProfile PrivateClusterProfile() {
  auto profile = api::MakeLocalEmulatorCloudProviderCapabilityProfile();
  profile.provider_profile_uuid = "00000000-0000-7000-8000-000000000120";
  profile.display_name = "local_emulator_with_private_cluster_fields";
  profile.private_cluster_only_fields = {
      "cluster.route_epoch",
      "cluster_fence_token",
      "private_kms_partition_policy",
  };
  return profile;
}

void TestLocalEmulatorSuccess() {
  auto registry = api::MakeCloudProviderCapabilityRegistryWithLocalEmulator();
  const auto deployment = api::MakeLocalEmulatorCloudDeploymentProfile();
  const auto result = api::ValidateCloudDeploymentProfile(registry, deployment);

  Require(result.ok, "local emulator deployment should validate");
  Require(result.profile_selector_valid, "local emulator selector should be valid");
  Require(result.selected_provider_found, "local emulator provider should be selected");
  Require(!result.side_effects_performed, "capability validation must be side-effect-free");
  Require(result.mga_authority_preserved, "MGA authority flag should be preserved");
  Require(!result.provider_state_finality_authority,
          "provider state must not be finality authority");
  Require(!result.provider_recovery_authority,
          "provider state must not be recovery authority");
  Require(!result.parser_finality_authority,
          "parser must not be finality authority");
}

void TestMissingProviderFailsClosed() {
  auto registry = api::MakeCloudProviderCapabilityRegistryWithLocalEmulator();
  auto deployment = api::MakeLocalEmulatorCloudDeploymentProfile(
      "00000000-0000-7000-8000-000000009999");
  const auto result = api::ValidateCloudDeploymentProfile(registry, deployment);

  Require(!result.ok, "missing provider should fail closed");
  Require(!result.side_effects_performed, "missing provider must be side-effect-free");
  Require(HasDiagnostic(result.diagnostics, api::kCloudProviderDiagnosticProviderNotFound),
          "missing provider diagnostic not emitted");
}

void TestUnsupportedKmsFailsClosed() {
  auto registry = api::MakeCloudProviderCapabilityRegistryWithLocalEmulator();
  auto deployment = api::MakeLocalEmulatorCloudDeploymentProfile();
  deployment.requested_kms_modes.push_back(api::CloudKmsMode::cloud_kms);
  const auto result = api::ValidateCloudDeploymentProfile(registry, deployment);

  Require(!result.ok, "unsupported KMS mode should fail closed");
  Require(HasDiagnostic(result.diagnostics, api::kCloudProviderDiagnosticKmsModeUnsupported),
          "unsupported KMS diagnostic not emitted");
  Require(!result.side_effects_performed, "unsupported KMS must be side-effect-free");
}

void TestStaticSecretFailsClosed() {
  auto registry = api::MakeCloudProviderCapabilityRegistryWithLocalEmulator();
  auto deployment = api::MakeLocalEmulatorCloudDeploymentProfile();
  deployment.requested_identity_modes.push_back(
      api::CloudIdentityMode::static_secret_allowed_by_policy);
  deployment.static_secret_policy_authorized = false;
  const auto result = api::ValidateCloudDeploymentProfile(registry, deployment);

  Require(!result.ok, "static secret should fail closed without explicit policy");
  Require(HasDiagnostic(result.diagnostics,
                        api::kCloudProviderDiagnosticStaticSecretForbidden),
          "static secret refusal diagnostic not emitted");
  Require(!result.side_effects_performed, "static secret refusal must be side-effect-free");
}

void TestPublicClusterFieldFailsClosed() {
  auto registry = api::MakeCloudProviderCapabilityRegistryWithLocalEmulator();
  auto deployment = api::MakeLocalEmulatorCloudDeploymentProfile();
  deployment.private_cluster_only_fields.push_back("cluster.route_epoch");
  deployment.requested_route_modes.push_back(api::CloudRouteMode::cluster_route_refused);
  const auto result = api::ValidateCloudDeploymentProfile(registry, deployment);

  Require(!result.ok, "public cluster-only fields should fail closed");
  Require(HasDiagnostic(result.diagnostics,
                        api::kCloudProviderDiagnosticClusterFieldRefused),
          "cluster field refusal diagnostic not emitted");
  Require(!result.side_effects_performed,
          "cluster field refusal must be side-effect-free");
}

void TestDisabledProviderFailsClosed() {
  auto registry = api::MakeCloudProviderCapabilityRegistryWithLocalEmulator();
  auto disabled = api::MakeLocalEmulatorCloudProviderCapabilityProfile();
  disabled.provider_profile_uuid = "00000000-0000-7000-8000-000000000121";
  disabled.status = api::CloudProviderStatus::disabled;
  const auto registered = api::RegisterCloudProviderCapabilityProfile(&registry, disabled);
  Require(registered.ok, "disabled refusal profile should be registrable");

  auto deployment = api::MakeLocalEmulatorCloudDeploymentProfile(
      disabled.provider_profile_uuid);
  const auto result = api::ValidateCloudDeploymentProfile(registry, deployment);

  Require(!result.ok, "disabled provider should fail closed");
  Require(HasDiagnostic(result.diagnostics,
                        api::kCloudProviderDiagnosticProviderDisabled),
          "disabled provider diagnostic not emitted");
  Require(!result.side_effects_performed, "disabled provider must be side-effect-free");
}

void TestPublicProjectionRedactsPrivateClusterFields() {
  auto registry = api::MakeCloudProviderCapabilityRegistryWithLocalEmulator();
  const auto registered =
      api::RegisterCloudProviderCapabilityProfile(&registry, PrivateClusterProfile());
  Require(registered.ok, "private-cluster-field profile should register");

  api::CloudProviderCapabilityProjectionContext public_context;
  public_context.build_scope = api::CloudBuildScope::public_single_node;
  auto projection =
      api::BuildCloudProviderCapabilitySysInformationProjection(registry, public_context);
  Require(projection.ok, "public projection should succeed");
  Require(!projection.rows.empty(), "public projection should include provider rows");
  for (const auto& row : projection.rows) {
    Require(!RowHasField(row, "private_cluster_only_fields"),
            "public projection leaked private cluster-only field names");
  }

  public_context.include_private_cluster_fields = true;
  projection =
      api::BuildCloudProviderCapabilitySysInformationProjection(registry, public_context);
  Require(!projection.ok,
          "public projection request for private cluster fields should fail closed");
  Require(HasDiagnostic(projection.diagnostics,
                        api::kCloudProviderDiagnosticClusterFieldRefused),
          "projection cluster-field diagnostic not emitted");
}

void TestDuplicateRegistrationFailsClosed() {
  auto registry = api::MakeCloudProviderCapabilityRegistryWithLocalEmulator();
  const auto duplicate =
      api::RegisterCloudProviderCapabilityProfile(
          &registry, api::MakeLocalEmulatorCloudProviderCapabilityProfile());
  Require(!duplicate.ok, "duplicate provider UUID should fail closed");
  Require(HasDiagnostic(duplicate.diagnostics,
                        api::kCloudProviderDiagnosticCapabilityUnsupported),
          "duplicate provider diagnostic not emitted");
  Require(!duplicate.side_effects_performed,
          "duplicate registration refusal must be side-effect-free");
}

}  // namespace

int main() {
  TestLocalEmulatorSuccess();
  TestMissingProviderFailsClosed();
  TestUnsupportedKmsFailsClosed();
  TestStaticSecretFailsClosed();
  TestPublicClusterFieldFailsClosed();
  TestDisabledProviderFailsClosed();
  TestPublicProjectionRedactsPrivateClusterFields();
  TestDuplicateRegistrationFailsClosed();
  return EXIT_SUCCESS;
}
