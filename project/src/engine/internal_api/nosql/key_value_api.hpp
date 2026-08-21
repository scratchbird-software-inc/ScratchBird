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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

struct MgaRelationStorageDescriptor;

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

// RCP-075 engine-bound, read-only key/value model-source contract. The
// request carries query operands and immutable engine authority only; stored
// rows are read from the bound MGA relation and can never be supplied here.
enum class EngineBoundKeyValueReadOperationV1 : std::uint8_t {
  kGet = 1,
  kMultiGet = 2,
  kPrefixRange = 3,
};

struct EngineBoundKeyValueReadRequestV1 : EngineApiRequest {
  std::uint16_t abi_version{1};
  EngineBoundKeyValueReadOperationV1 operation{
      EngineBoundKeyValueReadOperationV1::kGet};
  std::string object_uuid;
  std::vector<EngineTypedValue> request_values;
  std::string statement_timestamp;
  std::string expected_descriptor_uuid;
  std::uint64_t expected_descriptor_generation{0};
  std::string selected_alternative_uuid;
  std::string capability_uuid;
  std::string provider_uuid;
  std::uint64_t provider_generation{0};
  std::size_t maximum_request_keys{0};
  std::uint64_t maximum_request_bytes{0};
  std::uint64_t maximum_scanned_row_versions{0};
  std::uint64_t maximum_decoded_bytes{0};
  std::size_t maximum_output_rows{0};
  std::uint64_t maximum_value_bytes{0};
  std::uint64_t maximum_result_bytes{0};
  std::uint64_t maximum_memory_bytes{0};
  bool exact_fallback_selected{false};
  std::function<bool()> cancellation_requested;
};

struct EngineBoundKeyValueRowV1 {
  std::string row_uuid;
  std::string key;
  std::string value;
};

struct EngineBoundKeyValueReadResultV1 : EngineApiResult {
  bool data_access_observed{false};
  bool exact_fallback_observed{false};
  bool residual_recheck_complete{false};
  bool base_row_mga_recheck_complete{false};
  bool security_recheck_complete{false};
  std::uint64_t scanned_row_version_count{0};
  std::uint64_t selected_visible_row_count{0};
  std::uint64_t result_byte_count{0};
  std::string ordering_id;
  std::string descriptor_uuid;
  std::uint64_t descriptor_generation{0};
  std::string selected_alternative_uuid;
  std::string capability_uuid;
  std::string provider_uuid;
  std::uint64_t provider_generation{0};
  std::vector<EngineBoundKeyValueRowV1> rows;
};

// Exact persisted storage-shape admission shared by the canonical query route
// and the engine-bound provider.  This is a read-only descriptor check; it
// performs no MGA row access and owns no snapshot or finality decisions.
bool ExactKeyValueStorageDescriptorV1(
    const MgaRelationStorageDescriptor& descriptor);

EngineBoundKeyValueReadResultV1 EngineBoundKeyValueReadV1(
    const EngineBoundKeyValueReadRequestV1& request);

}  // namespace scratchbird::engine::internal_api
