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

inline constexpr const char* kKeyValueExactKeyProofMissing =
    "SB_KV_EXACT_KEY_PHYSICAL_PROOF_MISSING";
inline constexpr const char* kKeyValuePrefixProofMissing =
    "SB_KV_PREFIX_PHYSICAL_PROOF_MISSING";
inline constexpr const char* kKeyValueTtlVisibilityProofMissing =
    "SB_KV_TTL_VISIBILITY_PROOF_MISSING";
inline constexpr const char* kKeyValuePipelineAdmissionRefused =
    "SB_KV_PIPELINE_BATCH_ADMISSION_REFUSED";
inline constexpr const char* kKeyValueAtomicProgramRefused =
    "SB_KV_ATOMIC_PROGRAM_REFUSED";

struct EngineKeyValuePhysicalProof {
  EngineNoSqlPhysicalProviderContract provider_contract;
  bool proof_supplied = false;
  bool exact_key_index_proof = false;
  bool prefix_index_proof = false;
  bool ttl_visibility_proof = false;
};

struct EngineKeyValueMutation {
  std::string key;
  std::string value;
  EngineApiU64 expires_after_local_transaction_id = 0;
};

struct EngineKeyValueAtomicStep {
  std::string opcode;
  std::string key;
  std::string operand;
};

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_NOSQL_KEY_VALUE_API
struct EngineKeyValueGetRequest : EngineApiRequest {
  std::string key;
  std::string prefix;
  EngineKeyValuePhysicalProof physical_proof;
};
struct EngineKeyValueGetResult : EngineApiResult {};
EngineKeyValueGetResult EngineKeyValueGet(const EngineKeyValueGetRequest& request);

struct EngineKeyValuePutRequest : EngineApiRequest {
  std::string key;
  std::string value;
  EngineApiU64 expires_after_local_transaction_id = 0;
};
struct EngineKeyValuePutResult : EngineApiResult {};
EngineKeyValuePutResult EngineKeyValuePut(const EngineKeyValuePutRequest& request);

struct EngineKeyValueMultiGetRequest : EngineApiRequest {
  std::vector<std::string> keys;
  EngineKeyValuePhysicalProof physical_proof;
};
struct EngineKeyValueMultiGetResult : EngineApiResult {};
EngineKeyValueMultiGetResult EngineKeyValueMultiGet(
    const EngineKeyValueMultiGetRequest& request);

struct EngineKeyValuePipelineRequest : EngineApiRequest {
  std::vector<EngineKeyValueMutation> puts;
  std::vector<std::string> get_keys;
  EngineKeyValuePhysicalProof physical_proof;
  EngineApiU64 max_admitted_operations = 0;
};
struct EngineKeyValuePipelineResult : EngineApiResult {};
EngineKeyValuePipelineResult EngineKeyValuePipeline(
    const EngineKeyValuePipelineRequest& request);

struct EngineKeyValueAtomicProgramRequest : EngineApiRequest {
  std::vector<EngineKeyValueAtomicStep> steps;
  EngineKeyValuePhysicalProof physical_proof;
};
struct EngineKeyValueAtomicProgramResult : EngineApiResult {};
EngineKeyValueAtomicProgramResult EngineKeyValueAtomicProgram(
    const EngineKeyValueAtomicProgramRequest& request);

}  // namespace scratchbird::engine::internal_api
