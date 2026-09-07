// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_procedural_body_runtime.hpp"

#include "core/hash/hash_digest.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace scratchbird::engine::sblr {
namespace {

constexpr std::string_view kNodeIdentity =
    "sblr.psql.node.psql_null_stmt.v1";
constexpr std::string_view kNodeDescriptor =
    "psql_null_stmt_ir_descriptor.v1";
constexpr std::string_view kNodeExecutor =
    "engine.psql.ir.psql_null_stmt";
constexpr std::string_view kNodeResult = "psql_ir_result.v1";
constexpr std::string_view kEffectSetDomain =
    "ScratchBird.PsqlNullStmtEffectSet.V1";
constexpr std::string_view kEvidenceDomain =
    "ScratchBird.SblrProceduralBody.V1";

void PutLe(std::vector<std::uint8_t>* out,
           std::uint64_t value,
           std::size_t width) {
  for (std::size_t i = 0; i < width; ++i) {
    out->push_back(static_cast<std::uint8_t>(value >> (i * 8)));
  }
}

std::uint64_t GetLe(const std::uint8_t* bytes, std::size_t width) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < width; ++i) {
    value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
  }
  return value;
}

template <class T>
bool Nonzero(const T& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

bool UuidV7(const ProceduralBodyUuid& value) {
  return Nonzero(value) && (value[6] & 0xf0u) == 0x70u &&
         (value[8] & 0xc0u) == 0x80u;
}

ProceduralBodySha256 HashText(std::string_view text) {
  const std::vector<std::uint8_t> bytes(text.begin(), text.end());
  return scratchbird::core::hash::ComputeSha256Digest(bytes).digest;
}

ProceduralBodySha256 HashEvidence(const std::uint8_t* bytes,
                                 std::size_t size) {
  std::vector<std::uint8_t> material(kEvidenceDomain.begin(),
                                     kEvidenceDomain.end());
  material.insert(material.end(), bytes, bytes + size);
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}

bool FixedHashesMatch(const SblrProceduralBodyV1& value) {
  return value.node_identity_sha256 == HashText(kNodeIdentity) &&
         value.node_descriptor_sha256 == HashText(kNodeDescriptor) &&
         value.node_executor_sha256 == HashText(kNodeExecutor) &&
         value.node_result_sha256 == HashText(kNodeResult) &&
         value.effect_set_sha256 == HashText(kEffectSetDomain);
}

bool ValidHeader(const std::uint8_t* bytes, std::size_t size) {
  return bytes != nullptr && size == kSblrProceduralBodyV1Bytes &&
         std::equal(bytes, bytes + 4, "SPBD") &&
         GetLe(bytes + 4, 2) == 1 &&
         GetLe(bytes + 6, 2) == kSblrProceduralBodyV1Bytes &&
         GetLe(bytes + 8, 4) == kSblrProceduralBodyV1Bytes &&
         std::all_of(bytes + 12, bytes + 16,
                     [](std::uint8_t byte) { return byte == 0; });
}

}  // namespace

SblrProceduralBodyV1 MakeSblrPsqlNullProceduralBodyV1(
    const ProceduralBodyUuid& body_uuid,
    std::uint64_t body_generation,
    const ProceduralBodyUuid& procedure_uuid,
    std::uint64_t procedure_generation,
    std::uint64_t node_executor_availability_generation) {
  SblrProceduralBodyV1 value;
  value.body_uuid = body_uuid;
  value.body_generation = body_generation;
  value.procedure_uuid = procedure_uuid;
  value.procedure_generation = procedure_generation;
  value.node_executor_availability_generation =
      node_executor_availability_generation;
  value.node_identity_sha256 = HashText(kNodeIdentity);
  value.node_descriptor_sha256 = HashText(kNodeDescriptor);
  value.node_executor_sha256 = HashText(kNodeExecutor);
  value.node_result_sha256 = HashText(kNodeResult);
  value.effect_set_sha256 = HashText(kEffectSetDomain);
  return value;
}

std::vector<std::uint8_t> EncodeSblrProceduralBodyV1(
    const SblrProceduralBodyV1& value) {
  if (!UuidV7(value.body_uuid) || value.body_generation == 0 ||
      !UuidV7(value.procedure_uuid) || value.procedure_generation == 0 ||
      value.body_uuid == value.procedure_uuid ||
      value.node_executor_availability_generation == 0 ||
      !FixedHashesMatch(value)) {
    return {};
  }

  std::vector<std::uint8_t> out{'S', 'P', 'B', 'D'};
  PutLe(&out, 1, 2);
  PutLe(&out, kSblrProceduralBodyV1Bytes, 2);
  PutLe(&out, kSblrProceduralBodyV1Bytes, 4);
  PutLe(&out, 0, 4);
  out.insert(out.end(), value.body_uuid.begin(), value.body_uuid.end());
  PutLe(&out, value.body_generation, 8);
  out.insert(out.end(), value.procedure_uuid.begin(),
             value.procedure_uuid.end());
  PutLe(&out, value.procedure_generation, 8);
  PutLe(&out, kProceduralBodyNullProfileV1, 2);
  PutLe(&out, 1, 2);  // node_count
  PutLe(&out, 1, 4);  // entry_node_ordinal
  PutLe(&out, 0, 4);  // body flags
  PutLe(&out, 1, 4);  // node ordinal
  PutLe(&out, kProceduralBodyPsqlNullNodeKindV1, 2);
  PutLe(&out, 1, 2);  // node version
  PutLe(&out, 0, 1);  // no control effect
  PutLe(&out, 0, 3);
  PutLe(&out, value.node_executor_availability_generation, 8);
  out.insert(out.end(), value.node_identity_sha256.begin(),
             value.node_identity_sha256.end());
  out.insert(out.end(), value.node_descriptor_sha256.begin(),
             value.node_descriptor_sha256.end());
  out.insert(out.end(), value.node_executor_sha256.begin(),
             value.node_executor_sha256.end());
  out.insert(out.end(), value.node_result_sha256.begin(),
             value.node_result_sha256.end());
  out.insert(out.end(), value.effect_set_sha256.begin(),
             value.effect_set_sha256.end());
  const auto evidence = HashEvidence(out.data() + 16, 240);
  if (Nonzero(value.evidence_sha256) && value.evidence_sha256 != evidence) {
    return {};
  }
  out.insert(out.end(), evidence.begin(), evidence.end());
  if (out.size() != kSblrProceduralBodyV1Bytes) {
    return {};
  }
  return out;
}

bool DecodeSblrProceduralBodyV1(const std::uint8_t* bytes,
                                std::size_t size,
                                SblrProceduralBodyV1* out,
                                std::string* detail) {
  const auto refuse = [&](std::string value) {
    if (detail != nullptr) {
      *detail = std::move(value);
    }
    return false;
  };
  if (out == nullptr || !ValidHeader(bytes, size)) {
    return refuse("SPBD header is not canonical");
  }
  if (GetLe(bytes + 64, 2) != kProceduralBodyNullProfileV1 ||
      GetLe(bytes + 66, 2) != 1 || GetLe(bytes + 68, 4) != 1 ||
      GetLe(bytes + 72, 4) != 0 || GetLe(bytes + 76, 4) != 1 ||
      GetLe(bytes + 80, 2) != kProceduralBodyPsqlNullNodeKindV1 ||
      GetLe(bytes + 82, 2) != 1 || bytes[84] != 0 ||
      std::any_of(bytes + 85, bytes + 88,
                  [](std::uint8_t byte) { return byte != 0; })) {
    return refuse("SPBD NULL-body shape is not canonical");
  }

  SblrProceduralBodyV1 value;
  std::copy_n(bytes + 16, 16, value.body_uuid.begin());
  value.body_generation = GetLe(bytes + 32, 8);
  std::copy_n(bytes + 40, 16, value.procedure_uuid.begin());
  value.procedure_generation = GetLe(bytes + 56, 8);
  value.node_executor_availability_generation = GetLe(bytes + 88, 8);
  std::copy_n(bytes + 96, 32, value.node_identity_sha256.begin());
  std::copy_n(bytes + 128, 32, value.node_descriptor_sha256.begin());
  std::copy_n(bytes + 160, 32, value.node_executor_sha256.begin());
  std::copy_n(bytes + 192, 32, value.node_result_sha256.begin());
  std::copy_n(bytes + 224, 32, value.effect_set_sha256.begin());
  std::copy_n(bytes + 256, 32, value.evidence_sha256.begin());

  if (!UuidV7(value.body_uuid) || value.body_generation == 0 ||
      !UuidV7(value.procedure_uuid) || value.procedure_generation == 0 ||
      value.body_uuid == value.procedure_uuid ||
      value.node_executor_availability_generation == 0) {
    return refuse("SPBD engine identity or generation is missing");
  }
  if (!FixedHashesMatch(value)) {
    return refuse("SPBD typed NULL-node identity is invalid");
  }
  const auto expected_evidence = HashEvidence(bytes + 16, 240);
  if (value.evidence_sha256 != expected_evidence) {
    return refuse("SPBD evidence is invalid");
  }

  const auto canonical = EncodeSblrProceduralBodyV1(value);
  if (canonical.size() != size ||
      !std::equal(canonical.begin(), canonical.end(), bytes)) {
    return refuse("SPBD canonical re-encoding differs");
  }
  *out = value;
  return true;
}

}  // namespace scratchbird::engine::sblr
