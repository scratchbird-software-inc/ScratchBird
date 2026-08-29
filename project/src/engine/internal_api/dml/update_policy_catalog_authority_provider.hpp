// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "dml/update_immutable_authority_provider.hpp"
#include "typed_update_carrier_codec.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_DML_UPDATE_POLICY_CATALOG_AUTHORITY_PROVIDER_V1
// Private bridge from the durable native policy catalog to exact DUSR/DUSV
// source authority.  It never derives policy semantics from predicate text,
// DUPV effective rows, names, or hashes.

struct EngineDmlUpdatePolicyCatalogCaptureRequestV1 {
  EngineRequestContext context;
  std::string authenticated_statement_receipt_uuid;
  std::uint64_t structural_occurrence_id = 0;
  EngineDmlUpdateRelationOccurrenceAuthorityV1 relation_occurrence;
  std::string catalog_snapshot_uuid;
  std::uint64_t catalog_generation = 0;
  std::string descriptor_uuid;
  std::uint64_t descriptor_generation = 0;
};

struct EngineDmlUpdatePolicyCatalogCaptureResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineSecurityPolicySnapshotAuthorityV1 security_policy_snapshot;
  std::vector<EngineDmlUpdateRowPolicyAuthoritySourceV1>
      immutable_policy_sources;
  scratchbird::wire::TypedUpdateSecurityPolicySourceVector
      source_policy_vector;
  std::vector<std::uint8_t> exact_source_policy_vector_dusv;
};

struct EngineDmlUpdateSecuritySnapshotProofRequestV1 {
  EngineRequestContext context;
  EngineDmlUpdatePolicyCatalogCaptureResultV1 captured;
  std::vector<std::uint8_t> exact_descriptor_dudc;
  std::vector<std::uint8_t> exact_row_policy_vector_dupv;
};

struct EngineDmlUpdateSecuritySnapshotProofResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  scratchbird::wire::TypedUpdateSecuritySnapshotProof proof;
  std::vector<std::uint8_t> exact_security_snapshot_proof_dusp;
};

EngineDmlUpdatePolicyCatalogCaptureResultV1
CaptureDmlUpdatePolicyCatalogAuthorityV1(
    const EngineDmlUpdatePolicyCatalogCaptureRequestV1& request);

EngineDmlUpdateSecuritySnapshotProofResultV1
BuildDmlUpdateSecuritySnapshotProofV1(
    const EngineDmlUpdateSecuritySnapshotProofRequestV1& request);

}  // namespace scratchbird::engine::internal_api
