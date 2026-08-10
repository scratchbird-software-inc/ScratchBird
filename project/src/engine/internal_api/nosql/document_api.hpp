// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "nosql/nosql_physical_provider_contract.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

inline constexpr const char* kDocumentExactPathProofMissing =
    "SB_DOCUMENT_EXACT_PATH_PHYSICAL_PROOF_MISSING";
inline constexpr const char* kDocumentWildcardShapeProofMissing =
    "SB_DOCUMENT_WILDCARD_SHAPE_PHYSICAL_PROOF_MISSING";
inline constexpr const char* kDocumentShapeDictionaryProofMissing =
    "SB_DOCUMENT_SHAPE_DICTIONARY_PROOF_MISSING";
inline constexpr const char* kDocumentStructuralSharingProofMissing =
    "SB_DOCUMENT_STRUCTURAL_SHARING_PROOF_MISSING";
inline constexpr const char* kDocumentPartialMaterializationProofMissing =
    "SB_DOCUMENT_PARTIAL_MATERIALIZATION_PROOF_MISSING";
inline constexpr const char* kDocumentPathIndexRuntimeUnproven =
    "SB_NOSQL_DOCUMENT_PATH_INDEX.INDEX_RUNTIME_UNPROVEN";
inline constexpr const char* kDocumentPathIndexUnavailable =
    "SB_NOSQL_DOCUMENT_PATH_INDEX.UNAVAILABLE";
inline constexpr const char* kDocumentProviderLifecycleUnsafe =
    "SB_NOSQL_PROVIDER_LIFECYCLE.UNSAFE";
inline constexpr const char* kModelDocumentExactFallbackUnavailable =
    "SB_MODEL_DOCUMENT_EXACT_FALLBACK_UNAVAILABLE_V1";
inline constexpr const char* kModelDocumentMissingBindingRefusedApi =
    "SB_MODEL_DOCUMENT_MISSING_BINDING_REFUSED_V1";

struct EngineDocumentPhysicalProof {
  EngineNoSqlPhysicalProviderContract provider_contract;
  bool proof_supplied = false;
  bool exact_path_index_proof = false;
  bool wildcard_shape_index_proof = false;
  bool shape_dictionary_proof = false;
  bool structural_sharing_proof = false;
  bool partial_materialization_proof = false;
  bool document_path_index_runtime_proven = false;
};

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_NOSQL_DOCUMENT_API
struct EngineDocumentInsertRequest : EngineApiRequest {
  // An engine-bound collection identity. Empty retains the legacy session
  // collection mapping for callers that have not entered the canonical model
  // source route.
  std::string collection_uuid;
  std::string document_uuid;
  std::string row_uuid;
};
struct EngineDocumentInsertResult : EngineApiResult {};
EngineDocumentInsertResult EngineDocumentInsert(const EngineDocumentInsertRequest& request);

struct EngineDocumentFindRequest : EngineApiRequest {
  std::string path;
  std::string equals_value;
  std::string comparison_operator{"="};
  EngineTypedValue comparison_value;
  bool comparison_value_present = false;
  bool wildcard_path = false;
  bool require_benchmark_clean_index_runtime = false;
  bool exact_collection_fallback = false;
  bool explicit_nullable_missing_projection = false;
  std::size_t maximum_rows = 65536;
  std::size_t maximum_cells = 16777216;
  std::uint64_t maximum_memory_bytes = 67108864;
  std::vector<std::string> projected_paths;
  std::vector<bool> projected_path_nullable;
  EngineDocumentPhysicalProof physical_proof;
};
struct EngineDocumentTypedPathValue {
  std::string path;
  EngineTypedValue value;
};
struct EngineDocumentTypedRow {
  std::string document_uuid;
  std::string row_uuid;
  std::vector<EngineDocumentTypedPathValue> values;
};
struct EngineDocumentFindResult : EngineApiResult {
  std::vector<EngineDocumentTypedRow> typed_rows;
  bool exact_collection_fallback_used = false;
  bool residual_recheck_complete = false;
  bool base_row_mga_recheck_complete = false;
  bool security_recheck_complete = false;
};
EngineDocumentFindResult EngineDocumentFind(const EngineDocumentFindRequest& request);

void EngineDocumentProviderCleanup(const EngineRequestContext& context,
                                   bool drop_persistent_state);

struct EngineDocumentUpdateRequest : EngineApiRequest {};
struct EngineDocumentUpdateResult : EngineApiResult {};
EngineDocumentUpdateResult EngineDocumentUpdate(const EngineDocumentUpdateRequest& request);

struct EngineDocumentDeleteRequest : EngineApiRequest {};
struct EngineDocumentDeleteResult : EngineApiResult {};
EngineDocumentDeleteResult EngineDocumentDelete(const EngineDocumentDeleteRequest& request);

}  // namespace scratchbird::engine::internal_api
