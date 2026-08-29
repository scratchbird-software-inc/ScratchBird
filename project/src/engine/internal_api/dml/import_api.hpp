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
// SB_ENGINE_INTERNAL_API_DML_IMPORT_API
// SB_PID007_IMPORT_SURFACE
//
// Import planning is an engine-owned API surface. Parsers may decode client
// syntax, client protocol packets, reference bulk APIs, and client-side file
// handles, but the engine only accepts canonical UUID/descriptors/policy
// envelopes and never executes SQL text.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "api_types.hpp"
#include "dml/import_reject_model.hpp"
#include "engine/sblr/sblr_plan_import_rows_codec.hpp"

namespace scratchbird::engine::internal_api {

struct EngineImportSourceEnvelope {
    std::string source_kind;
    EngineUuid source_uuid;
    std::string source_fingerprint;
    std::string source_position;
    std::string redacted_source_handle;
    bool source_handle_sensitive = true;
};

struct EngineImportFormatEnvelope {
    std::string format_family;
    std::string encoding;
    std::string line_ending;
    std::string delimiter;
    std::string quote;
    std::string escape;
    std::string header_policy;
    std::vector<std::string> null_markers;
    std::string date_time_profile;
    std::string timezone_profile;
    std::vector<std::pair<std::string, std::string>> format_options;
};

struct EngineImportColumnMapping {
    std::string source_field;
    std::string target_column;
    EngineDescriptor target_descriptor;
    bool required = false;
};

struct EngineImportPolicyEnvelope : public EngineImportRejectPolicyEnvelope {
    bool strict_bulk_load_requested = false;
    bool reference_relaxed_semantics_requested = false;
};

// SEARCH_KEY: SB_ENGINE_BOUND_IMPORT_DESCRIPTOR_REGISTRY_V1
// This is the engine-import-binder-owned immutable row retained by
// engine.bound_import_descriptor_registry.v1. The canonical carrier types and
// their numeric enum assignments are owned by the pure SBLR codec; the engine
// API deliberately does not duplicate them.
struct EngineBoundImportRowsPlanDescriptorV1 {
    scratchbird::engine::sblr::PlanImportRowsCarrierSetV1 carriers;
    scratchbird::engine::sblr::PlanImportRowsUuidV1 executor_evidence_uuid{};
    EngineApiU64 executor_evidence_generation = 0;
    bool reference_relaxed_semantics_authorized = false;
};

struct EngineBindImportRowsPlanDescriptorRequestV1 {
    EngineRequestContext context;
    EngineApiU64 structural_occurrence_id = 0;
    scratchbird::engine::sblr::PlanImportRowsLiveAuthorityV1 live_authority;
    EngineBoundImportRowsPlanDescriptorV1 row;
};

struct EngineBindImportRowsPlanDescriptorResultV1 {
    bool ok = false;
    scratchbird::engine::sblr::PlanImportRowsDescriptorRefV1 descriptor_ref;
    EngineApiDiagnostic diagnostic;
};

// Generation-free semantic mapping demand. The authenticated engine binder
// supplies only an exact target-column UUID and structural source ordinal.
// Core v1 has no admitted numeric projection for IMAP codec_id_u16, so the
// production factory accepts an empty vector and refuses every structurally
// valid nonempty vector as SBLR.OPERATION_UNSUPPORTED without publication.
struct EngineImportRowsColumnMappingDemandV1 {
    std::uint32_t source_field_ordinal = 0;
    bool required = false;
    EngineUuid target_column_uuid;
};

// Semantic binder demand. Every enum and authority identity is explicit; zero
// values are invalid and no source, format, mapping, or policy default exists.
// The factory issues only operation-owned row/request/evidence identities.
struct EngineCreateImportRowsPlanDescriptorRequestV1 {
    EngineRequestContext context;
    EngineApiU64 structural_occurrence_id = 0;
    // This is an exact UUID resolved by the engine import binder. The factory
    // loads the current MGA relation descriptor itself; no localized name,
    // parser storage identity, or caller-composed live-authority projection is
    // accepted here.
    EngineUuid target_table_uuid;

    scratchbird::engine::sblr::PlanImportRowsSourceKindV1 source_kind =
        static_cast<scratchbird::engine::sblr::PlanImportRowsSourceKindV1>(0);
    bool source_fingerprint_present = false;
    scratchbird::engine::sblr::PlanImportRowsSha256V1
        source_fingerprint_sha256{};

    scratchbird::engine::sblr::PlanImportRowsFormatFamilyV1 format_family =
        static_cast<scratchbird::engine::sblr::PlanImportRowsFormatFamilyV1>(0);
    std::vector<EngineImportRowsColumnMappingDemandV1> mappings;

    scratchbird::engine::sblr::PlanImportRowsRejectModeV1 reject_mode =
        static_cast<scratchbird::engine::sblr::PlanImportRowsRejectModeV1>(0);
    scratchbird::engine::sblr::PlanImportRowsRejectPayloadPolicyV1
        reject_payload_policy = static_cast<scratchbird::engine::sblr::
            PlanImportRowsRejectPayloadPolicyV1>(0);
    scratchbird::engine::sblr::PlanImportRowsResumePolicyV1 resume_policy =
        static_cast<scratchbird::engine::sblr::PlanImportRowsResumePolicyV1>(0);
    bool strict_bulk_load_requested = false;
    bool reference_relaxed_semantics_requested = false;
    bool reference_relaxed_semantics_authorized = false;
    std::uint32_t reject_limit_ppm = 0;
    EngineApiU64 reject_limit_rows = 0;
    scratchbird::engine::sblr::PlanImportRowsUuidV1
        reject_target_relation_uuid{};
    EngineApiU64 reject_target_relation_generation = 0;
    scratchbird::engine::sblr::PlanImportRowsSha256V1
        reject_target_relation_sha256{};
};

struct EngineReleaseImportRowsPlanDescriptorsResultV1 {
    bool ok = false;
    EngineApiU64 released_row_count = 0;
    EngineApiDiagnostic diagnostic;
};

EngineBindImportRowsPlanDescriptorResultV1
PublishEngineBoundImportRowsPlanDescriptorV1(
    const EngineBindImportRowsPlanDescriptorRequestV1& request);

EngineBindImportRowsPlanDescriptorResultV1
CreateAndPublishEngineBoundImportRowsPlanDescriptorV1(
    const EngineCreateImportRowsPlanDescriptorRequestV1& request);

EngineReleaseImportRowsPlanDescriptorsResultV1
ReleaseEngineBoundImportRowsPlanDescriptorsV1(
    const EngineRequestContext& context);

struct EnginePlanImportRowsRequest : public EngineApiRequest {
    // The strict planning ingress carries exactly one SBOP v1 descriptor
    // reference. Structural occurrence and every live authority field are
    // recovered from the immutable engine registry and authoritative engine
    // loaders; neither is decoded from names, option strings, operand ordinal,
    // or localized parser metadata.
    scratchbird::engine::sblr::PlanImportRowsDescriptorRefV1 descriptor_ref;

    // Conditional cluster execution evidence is engine/gateway authority and
    // is never populated from the plan descriptor. Standalone requests leave
    // all three fields false.
    bool cluster_context_execution_validated = false;
    bool cluster_read_route_security_validated = false;
    bool cluster_read_only_evidence_validated = false;

    // Legacy in-process execution callers still compile against these fields.
    // EnginePlanImportRows never treats them as planning authority: a strict
    // request must resolve the immutable bound descriptor above, and any
    // localized-name or option-based authority is refused.
    EngineObjectReference target_table;
    EngineImportSourceEnvelope source;
    EngineImportFormatEnvelope format;
    std::vector<EngineImportColumnMapping> column_mappings;
    EngineImportPolicyEnvelope import_policy;
};

struct EnginePlanImportRowsResult : public EngineApiResult {
    bool surface_accepted = false;
    bool planning_only = false;
    bool execution_requires_execute_import_rows = false;
    bool row_execution_completed = false;
    bool row_persistence_claimed = false;

    // Exact v1 result enum codes. The text fields below are retained only for
    // existing internal execute-import source compatibility and are not part
    // of the canonical result extension carried by the public ABI.
    std::uint16_t normalized_insert_mode_code = 0;
    std::uint16_t normalized_source_kind_code = 0;
    std::uint16_t normalized_format_family_code = 0;
    std::string normalized_insert_mode;
    std::string normalized_source_kind;
    std::string normalized_format_family;
    EngineApiU64 mapped_column_count = 0;
    EngineUuid validated_request_descriptor_uuid;
    EngineApiU64 validated_request_descriptor_generation = 0;
    std::array<std::uint8_t, 32> validated_request_projection_sha256{};
    scratchbird::engine::sblr::PlanImportRowsExecutorEvidenceV1
        accepted_executor_evidence;
};

EnginePlanImportRowsResult EnginePlanImportRows(const EnginePlanImportRowsRequest& request);

} // namespace scratchbird::engine::internal_api
