// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CLOUD_IDENTITY_KMS
// Engine-owned cloud identity and KMS policy validation. Providers and local
// emulators supply evidence only; plaintext material is never persisted or
// returned by this API.

struct CloudProtectedReference {
  EngineUuid reference_uuid;
  std::string reference_kind;
  std::string provider_profile_uuid;
  std::string protected_material_uuid;
  std::string protected_material_version_uuid;
  std::string redacted_external_reference;
};

struct CloudKmsEnvelopeMetadata {
  EngineUuid envelope_uuid;
  EngineUuid wrapping_reference_uuid;
  std::string kms_profile_uuid;
  std::string kms_mode;
  std::string envelope_version;
  std::string rotation_policy_uuid;
  std::string audit_policy_uuid;
  bool plaintext_material_persisted = false;
  bool plaintext_material_returned = false;
};

struct CloudIdentityKmsValidation {
  bool ok = false;
  std::string identity_mode;
  std::string kms_mode;
  CloudProtectedReference identity_reference;
  CloudProtectedReference kms_reference;
  CloudKmsEnvelopeMetadata envelope;
  EngineApiDiagnostic diagnostic;
  std::vector<EngineEvidenceReference> evidence;
  std::vector<std::pair<std::string, std::string>> rows;
};

std::string CanonicalCloudIdentityMode(std::string mode);
std::string CanonicalCloudKmsMode(std::string mode);
bool CloudIdentityModeIsSecretless(const std::string& mode);
CloudIdentityKmsValidation ValidateCloudIdentityKmsPolicy(const EngineApiRequest& request);
EngineApiResult ValidateCloudIdentityKmsPolicyApi(const EngineApiRequest& request);

}  // namespace scratchbird::engine::internal_api
