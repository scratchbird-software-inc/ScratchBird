// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

struct SblrExpressionLiteralNodeV1 {
  std::uint64_t node_id = 0;
  std::uint64_t parent_node_id = 0;
  std::uint32_t parent_operand_ordinal = 0;
  std::uint64_t descriptor_generation = 0;
  std::array<std::uint8_t, 16> descriptor_uuid{};
  std::vector<std::uint8_t> literal_body;
};

struct SblrExpressionNodeTableV1 {
  std::vector<SblrExpressionLiteralNodeV1> nodes;
};

struct SblrExpressionNodeTableCodecResultV1 {
  bool ok = false;
  std::string diagnostic_id;
  std::string detail;
  SblrExpressionNodeTableV1 table;
  std::vector<std::uint8_t> canonical_bytes;
};

inline constexpr std::size_t kSblrExpressionNodeTableMaximumBytesV1 =
    610336;
inline constexpr std::size_t
    kSblrContextualComposedExpressionNodeTableMaximumBytesV2 = 642875;

struct SblrExpressionNodeReferenceV1 {
  std::uint32_t occurrence_ordinal = 0;
  std::uint64_t node_id = 0;
  std::array<std::uint8_t, 32> node_table_sha256{};
  std::array<std::uint8_t, 16> descriptor_uuid{};
  std::uint64_t descriptor_generation = 0;
};

struct SblrLiteralStatementDescriptorProfileV1 {
  std::array<std::uint8_t, 16> profile_uuid{};
  std::array<std::uint8_t, 16> statement_receipt_uuid{};
  std::array<std::uint8_t, 16> catalog_snapshot_uuid{};
  std::uint64_t catalog_generation = 0;
  std::array<std::uint8_t, 16> descriptor_uuid{};
  std::uint64_t descriptor_generation = 0;
  std::array<std::uint8_t, 16> type_uuid{};
  std::string codec_id;
  std::uint16_t codec_version = 0;
  std::uint64_t codec_generation = 0;
  bool nullable = false;
  std::array<std::uint8_t, 32> profile_binding_sha256{};
};

struct SblrLiteralDescriptorProfileCodecResultV1 {
  bool ok = false;
  std::string diagnostic_id;
  std::string detail;
  SblrLiteralStatementDescriptorProfileV1 profile;
  std::vector<std::uint8_t> canonical_bytes;
};

struct SblrLiteralDemandV1 {
  std::uint64_t occurrence_id = 0;
  std::uint16_t lexical_class = 0;
  std::uint16_t context_class = 0;
  bool nullable = false;
  std::array<std::uint8_t, 32> lexical_sha256{};
};

struct SblrLiteralPrebindRequestV1 {
  std::array<std::uint8_t,16> preliminary_receipt_uuid{};
  std::array<std::uint8_t,16> catalog_snapshot_uuid{};
  std::uint64_t catalog_generation=0;
  std::uint64_t security_epoch=0;
  std::uint64_t resource_epoch=0;
  std::array<std::uint8_t,16> mga_snapshot_uuid{};
  std::vector<SblrLiteralDemandV1> demands;
  std::array<std::uint8_t,32> demand_sha256{};
};

struct SblrLiteralPrebindRequestCodecResultV1 {
  bool ok=false;
  std::string diagnostic_id;
  std::string detail;
  SblrLiteralPrebindRequestV1 request;
  std::vector<std::uint8_t> canonical_bytes;
};

std::array<std::uint8_t,32> ComputeSblrLiteralDemandSequenceSha256V1(
    const std::vector<SblrLiteralDemandV1>& demands);
std::vector<std::uint8_t> EncodeSblrLiteralPrebindRequestV1(
    const SblrLiteralPrebindRequestV1& request);
SblrLiteralPrebindRequestCodecResultV1 DecodeSblrLiteralPrebindRequestV1(
    const std::uint8_t* bytes,std::size_t size);

struct SblrLiteralProfileMappingV1 {
  std::uint64_t occurrence_id=0;
  std::vector<std::uint8_t> sblp_bytes;
};
std::array<std::uint8_t,32> ComputeSblrLiteralOrderedProfilesSha256V1(
    const std::vector<SblrLiteralProfileMappingV1>& mappings);
struct SblrLiteralPrebindResultV1 {
  std::array<std::uint8_t,16> preliminary_receipt_uuid{};
  std::array<std::uint8_t,16> catalog_snapshot_uuid{};
  std::uint64_t catalog_generation=0,security_epoch=0,resource_epoch=0;
  std::array<std::uint8_t,16> mga_snapshot_uuid{};
  std::array<std::uint8_t,32> demand_sha256{};
  std::vector<SblrLiteralProfileMappingV1> mappings;
  std::array<std::uint8_t,32> ordered_profile_sha256{};
};
std::vector<std::uint8_t> EncodeSblrLiteralPrebindResultV1(
    const SblrLiteralPrebindResultV1& result);
struct SblrLiteralFinalizeRequestV1 {
  std::array<std::uint8_t,16> preliminary_receipt_uuid{};
  std::array<std::uint8_t,32> demand_sha256{},ordered_profile_sha256{},bound_ast_sha256{},sbxn_sha256{};
  std::uint64_t catalog_generation=0,security_epoch=0,resource_epoch=0;
  std::array<std::uint8_t,16> mga_snapshot_uuid{};
  std::vector<std::uint8_t> canonical_sbba;
  std::vector<std::uint8_t> canonical_sbxn;
};
bool DecodeSblrLiteralFinalizeRequestV1(const std::uint8_t* bytes,
                                        std::size_t size,
                                        SblrLiteralFinalizeRequestV1* out);
struct SblrLiteralAdmissionV1 {
  std::array<std::uint8_t,16> preliminary_receipt_uuid{},final_receipt_uuid{},admission_token_uuid{};
  std::array<std::uint8_t,32> demand_sha256{},ordered_profile_sha256{},bound_ast_sha256{},sbxn_sha256{};
  std::uint64_t catalog_generation=0,security_epoch=0,resource_epoch=0;
  std::array<std::uint8_t,16> mga_snapshot_uuid{};
  std::array<std::uint8_t,32> admission_token_binding_sha256{};
};
std::vector<std::uint8_t> EncodeSblrLiteralAdmissionV1(
    SblrLiteralAdmissionV1* admission);
struct SblrLiteralBoundAstNodeV1 {
  std::uint32_t parent_operand_ordinal=0;std::uint64_t node_id=0;
  std::array<std::uint8_t,16> descriptor_uuid{};std::uint64_t descriptor_generation=0;
  std::array<std::uint8_t,16> type_uuid{},profile_uuid{};std::uint64_t occurrence_id=0;
  std::array<std::uint8_t,32> lexical_sha256{};bool nullable=false;
};
struct SblrLiteralBoundAstV1 {std::array<std::uint8_t,16> preliminary_receipt_uuid{};std::array<std::uint8_t,32> demand_sha256{};std::vector<SblrLiteralBoundAstNodeV1> nodes;};
std::vector<std::uint8_t> EncodeSblrLiteralBoundAstV1(const SblrLiteralBoundAstV1& value);
bool DecodeSblrLiteralBoundAstV1(const std::uint8_t* bytes,std::size_t size,SblrLiteralBoundAstV1* out);
std::array<std::uint8_t,32> ComputeSblrLiteralBoundAstSha256V1(const std::vector<std::uint8_t>& canonical_sbba);

// Exact demand-code authority for the current admitted tranche. This is a
// numeric registry lookup, not an implementation enum or spelling inference.
inline constexpr bool IsAdmittedBigintLiteralDemandV1(
    const SblrLiteralDemandV1& demand) {
  return demand.lexical_class==1 && demand.context_class==1 &&
         !demand.nullable;
}

inline constexpr bool IsAdmittedExactDecimalLiteralDemandV1(
    const SblrLiteralDemandV1& demand) {
  return demand.lexical_class==2 && demand.context_class==1 &&
         !demand.nullable;
}

inline constexpr bool IsAdmittedLiteralDemandV1(
    const SblrLiteralDemandV1& demand) {
  return IsAdmittedBigintLiteralDemandV1(demand) ||
         IsAdmittedExactDecimalLiteralDemandV1(demand);
}

std::array<std::uint8_t, 32> ComputeSblrLiteralDescriptorProfileBindingV1(
    const SblrLiteralStatementDescriptorProfileV1& profile,
    std::uint64_t receipt_security_epoch,
    std::uint64_t receipt_resource_epoch);
std::vector<std::uint8_t> EncodeSblrLiteralDescriptorProfileV1(
    const SblrLiteralStatementDescriptorProfileV1& profile);
SblrLiteralDescriptorProfileCodecResultV1
DecodeSblrLiteralDescriptorProfileV1(const std::uint8_t* bytes,
                                     std::size_t size);

inline constexpr std::string_view kSblrLiteralInt64LeCodecId =
    "datatype.int64.le.v1";
inline constexpr std::uint16_t kSblrLiteralInt64LeCodecVersion = 1;
inline constexpr std::string_view kSblrLiteralExactDecimalCodecId =
    "datatype.decimal.base1e9.le.v1";
inline constexpr std::uint16_t kSblrLiteralExactDecimalCodecVersion = 1;
inline constexpr std::size_t kSblrLiteralExactDecimalBytes = 24;

struct SblrLiteralExactDecimalCodecResultV1 {
  bool ok = false;
  std::string diagnostic_id;
  std::string detail;
  std::uint8_t precision = 0;
  std::uint8_t scale = 0;
  std::string canonical_lexical;
  std::array<std::uint8_t, kSblrLiteralExactDecimalBytes> canonical_bytes{};
};

struct SblrLiteralExecutorEvidenceV1 {
  std::string executor_id = "engine.op.literal";
  std::uint16_t opcode_code = 3;
  std::string opcode_version = "1.0";
  std::string operand_descriptor_id = "typed_literal";
  std::array<std::uint8_t,16> descriptor_uuid{};
  std::uint64_t descriptor_generation = 0;
  std::array<std::uint8_t,32> canonical_value_sha256{};
  std::string result_descriptor_id = "typed_value";
  std::uint16_t result_descriptor_version = 1;
};
std::optional<std::array<std::uint8_t,32>>
ComputeSblrLiteralExecutorEvidenceSha256V1(
    const SblrLiteralExecutorEvidenceV1& evidence);

// The Core bigint codec is deliberately narrow: exactly eight signed
// two's-complement little-endian bytes.  No text parsing, widening, profile
// guessing, or host-endian reinterpretation is admitted here.
std::optional<std::int64_t> DecodeSblrLiteralInt64LeV1(
    const std::uint8_t* bytes, std::size_t size);
std::array<std::uint8_t, 8> EncodeSblrLiteralInt64LeV1(std::int64_t value);

// Exact decimal literals use the manifest-admitted 24-byte base-1e9 codec.
// The encoder validates and canonicalizes lexical input through sbl_numeric;
// the decoder reconstructs the exact fixed-point spelling and requires a
// canonical decode/re-encode identity. Neither API uses a binary float.
SblrLiteralExactDecimalCodecResultV1 EncodeSblrLiteralExactDecimalV1(
    std::string_view lexical);
SblrLiteralExactDecimalCodecResultV1 DecodeSblrLiteralExactDecimalV1(
    const std::uint8_t* bytes, std::size_t size);

bool DecodeSblrExpressionNodeReferenceV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrExpressionNodeReferenceV1* out);
bool ValidateSblrLiteralReferenceBijectionV1(
    const SblrExpressionNodeTableCodecResultV1& table,
    const std::vector<SblrExpressionNodeReferenceV1>& references);

SblrExpressionNodeTableCodecResultV1 DecodeSblrExpressionNodeTableV1(
    const std::uint8_t* bytes, std::size_t size);
SblrExpressionNodeTableCodecResultV1
DecodeSblrContextualComposedExpressionNodeTableV2(
    const std::uint8_t* bytes, std::size_t size);
std::vector<std::uint8_t> EncodeSblrExpressionNodeTableV1(
    const SblrExpressionNodeTableV1& table);

}  // namespace scratchbird::engine::sblr
