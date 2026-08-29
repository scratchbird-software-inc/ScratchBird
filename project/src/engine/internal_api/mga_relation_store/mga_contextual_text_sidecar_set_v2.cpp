// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_contextual_text_sidecar_set_v2.hpp"

#include "hash_digest.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

using Descriptor = sblr::ContextualTextDescriptorV2;
using FieldPair = MgaContextualTextDescriptorFieldPairV2;
using RawBytes = MgaContextualTextRawBytesV2;
using Sha256 = MgaContextualTextSha256V2;
using Uuid = MgaContextualTextUuidV2;

static_assert(kMgaContextualTextSidecarSetSealKeyV2.size() == 53);
static_assert(2 * kMgaContextualTextSidecarSetSealKeyV2.size() + 1 +
                      2 * Sha256{}.size() ==
                  171);

bool Fail(MgaContextualTextSidecarSetDiagnosticV2* diagnostic,
          std::string code,
          std::string detail) {
  if (diagnostic != nullptr) {
    diagnostic->code = std::move(code);
    diagnostic->detail = std::move(detail);
  }
  return false;
}

bool Invalid(MgaContextualTextSidecarSetDiagnosticV2* diagnostic,
             std::string detail) {
  return Fail(diagnostic, "CTB.TEXT.DESCRIPTOR_INVALID", std::move(detail));
}

void Clear(MgaContextualTextSidecarSetDiagnosticV2* diagnostic) {
  if (diagnostic != nullptr) *diagnostic = {};
}

bool Nonzero(const Uuid& value) {
  return std::any_of(value.begin(), value.end(),
                     [](const std::uint8_t byte) { return byte != 0; });
}

bool Nonzero(const Sha256& value) {
  return std::any_of(value.begin(), value.end(),
                     [](const std::uint8_t byte) { return byte != 0; });
}

RawBytes Raw(std::string_view value) {
  return RawBytes(value.begin(), value.end());
}

std::string Text(const RawBytes& value) {
  return std::string(value.begin(), value.end());
}

std::string LowerAscii(std::string value) {
  for (char& byte : value) {
    if (byte >= 'A' && byte <= 'Z') {
      byte = static_cast<char>(byte - 'A' + 'a');
    }
  }
  return value;
}

bool CheckedAdd(std::uint64_t left,
                std::uint64_t right,
                std::uint64_t* out) {
  if (out == nullptr ||
      right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  *out = left + right;
  return true;
}

bool CheckedDouble(std::uint64_t value, std::uint64_t* out) {
  if (out == nullptr ||
      value > std::numeric_limits<std::uint64_t>::max() / 2) {
    return false;
  }
  *out = value * 2;
  return true;
}

bool SizeAsU64(std::size_t value, std::uint64_t* out) {
  if (out == nullptr) return false;
  if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
    if (value > std::numeric_limits<std::uint64_t>::max()) return false;
  }
  *out = static_cast<std::uint64_t>(value);
  return true;
}

void AppendU32(RawBytes* bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    bytes->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void AppendU64(RawBytes* bytes, std::uint64_t value) {
  for (unsigned shift = 0; shift != 64; shift += 8) {
    bytes->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

template <std::size_t N>
void AppendArray(RawBytes* bytes, const std::array<std::uint8_t, N>& value) {
  bytes->insert(bytes->end(), value.begin(), value.end());
}

void AppendDomain(RawBytes* bytes, std::string_view domain) {
  bytes->insert(bytes->end(), domain.begin(), domain.end());
  bytes->push_back(0);
}

void AppendHex(RawBytes* bytes, const RawBytes& raw) {
  static constexpr char kHex[] = "0123456789abcdef";
  for (const std::uint8_t byte : raw) {
    bytes->push_back(static_cast<std::uint8_t>(kHex[byte >> 4]));
    bytes->push_back(static_cast<std::uint8_t>(kHex[byte & 0x0f]));
  }
}

bool Digest(const RawBytes& material,
            Sha256* out,
            MgaContextualTextSidecarSetDiagnosticV2* diagnostic) {
  if (out == nullptr) return Invalid(diagnostic, "SHA-256 output is null");
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(material);
  if (!digest.ok() ||
      digest.digest_bytes != scratchbird::core::hash::kSha256DigestBytes) {
    return Fail(diagnostic, "SBLR.EXECUTION_FAILED",
                "contextual TEXT sidecar-set SHA-256 failed");
  }
  std::copy(digest.digest.begin(), digest.digest.end(), out->begin());
  return true;
}

bool ValidOwner(const MgaContextualTextSidecarSetOwnerV2& owner,
                MgaContextualTextSidecarSetDiagnosticV2* diagnostic) {
  if (owner.creator_transaction_id == 0 || owner.event_sequence == 0 ||
      !Nonzero(owner.relation_uuid) ||
      !Nonzero(owner.relation_descriptor_uuid) ||
      owner.relation_descriptor_generation == 0) {
    return Invalid(diagnostic,
                   "contextual TEXT sidecar-set owner is incomplete");
  }
  return true;
}

std::string BlobKey(std::uint32_t ordinal) {
  return "column." + std::to_string(ordinal) +
         std::string(kMgaContextualTextSidecarBlobSuffixV2);
}

std::string HashKey(std::uint32_t ordinal) {
  return "column." + std::to_string(ordinal) +
         std::string(kMgaContextualTextSidecarHashSuffixV2);
}

bool LooksContextualReserved(std::string_view key) {
  const std::string lower = LowerAscii(std::string(key));
  return lower == LowerAscii(std::string(kMgaContextualTextSidecarSetSealKeyV2)) ||
         lower.find("contextual_text_descriptor_sidecar_v2") !=
             std::string::npos;
}

enum class KeyKind {
  kBase,
  kBlob,
  kHash,
  kFinalSeal,
  kMalformedReserved,
};

struct ParsedKey {
  KeyKind kind = KeyKind::kBase;
  std::uint32_t ordinal = 0;
};

ParsedKey ParseKey(const RawBytes& raw_key) {
  const std::string key = Text(raw_key);
  if (key == kMgaContextualTextSidecarSetSealKeyV2) {
    return {KeyKind::kFinalSeal, 0};
  }

  const auto parse_ordinal = [&](std::string_view suffix,
                                 KeyKind kind) -> ParsedKey {
    constexpr std::string_view kPrefix = "column.";
    if (!key.starts_with(kPrefix) || !key.ends_with(suffix) ||
        key.size() <= kPrefix.size() + suffix.size()) {
      return {KeyKind::kMalformedReserved, 0};
    }
    const std::string_view digits(key.data() + kPrefix.size(),
                                  key.size() - kPrefix.size() - suffix.size());
    if (digits.empty() || (digits.size() > 1 && digits.front() == '0')) {
      return {KeyKind::kMalformedReserved, 0};
    }
    std::uint64_t ordinal = 0;
    for (const char digit : digits) {
      if (digit < '0' || digit > '9') {
        return {KeyKind::kMalformedReserved, 0};
      }
      const std::uint64_t next_digit =
          static_cast<std::uint64_t>(digit - '0');
      if (ordinal > (std::numeric_limits<std::uint32_t>::max() - next_digit) /
                        10) {
        return {KeyKind::kMalformedReserved, 0};
      }
      ordinal = ordinal * 10 + next_digit;
    }
    const auto canonical = kind == KeyKind::kBlob
                               ? BlobKey(static_cast<std::uint32_t>(ordinal))
                               : HashKey(static_cast<std::uint32_t>(ordinal));
    if (canonical != key) return {KeyKind::kMalformedReserved, 0};
    return {kind, static_cast<std::uint32_t>(ordinal)};
  };

  if (key.ends_with(kMgaContextualTextSidecarHashSuffixV2)) {
    return parse_ordinal(kMgaContextualTextSidecarHashSuffixV2,
                         KeyKind::kHash);
  }
  if (key.ends_with(kMgaContextualTextSidecarBlobSuffixV2)) {
    return parse_ordinal(kMgaContextualTextSidecarBlobSuffixV2,
                         KeyKind::kBlob);
  }
  if (LooksContextualReserved(key)) {
    return {KeyKind::kMalformedReserved, 0};
  }
  return {KeyKind::kBase, 0};
}

bool ValidateBaseFields(
    std::span<const FieldPair> fields,
    MgaContextualTextSidecarSetDiagnosticV2* diagnostic) {
  if (fields.empty()) {
    return Invalid(diagnostic,
                   "contextual TEXT pre-seal base field vector is empty");
  }
  std::set<RawBytes> keys;
  for (const auto& field : fields) {
    if (field.key_raw_bytes.empty()) {
      return Invalid(diagnostic, "descriptor field key is empty");
    }
    if (ParseKey(field.key_raw_bytes).kind != KeyKind::kBase) {
      return Invalid(
          diagnostic,
          "base descriptor fields contain a contextual reserved key");
    }
    if (!keys.insert(field.key_raw_bytes).second) {
      return Invalid(diagnostic, "base descriptor field key is duplicated");
    }
  }
  return true;
}

bool EncodeExpectedDescriptor(
    const MgaContextualTextProjectedColumnV2& column,
    RawBytes* exact_blob,
    Sha256* descriptor_evidence,
    MgaContextualTextSidecarSetDiagnosticV2* diagnostic) {
  if (!column.comparable_persisted_text) {
    return Invalid(diagnostic,
                   "noncomparable column requested a contextual sidecar");
  }
  const Descriptor& descriptor = column.expected_text_descriptor;
  if (!Nonzero(column.column_uuid) ||
      column.projected_datatype_descriptor_generation != 1 ||
      column.projected_datatype_catalog_generation != 1 ||
      column.projected_datatype_registry_generation != 1 ||
      column.projected_resource_epoch == 0 ||
      descriptor.descriptor_uuid !=
          column.projected_datatype_descriptor_uuid ||
      descriptor.descriptor_generation !=
          column.projected_datatype_descriptor_generation ||
      descriptor.datatype_catalog_snapshot_uuid !=
          column.projected_datatype_catalog_snapshot_uuid ||
      descriptor.datatype_catalog_generation !=
          column.projected_datatype_catalog_generation ||
      descriptor.datatype_registry_generation !=
          column.projected_datatype_registry_generation ||
      descriptor.resource_epoch != column.projected_resource_epoch) {
    return Invalid(
        diagnostic,
        "projected d701/1/1, d718/1, or resource epoch does not match SBTLTD02");
  }

  sblr::ContextualTextCodecDiagnosticV2 codec_diagnostic;
  RawBytes canonical;
  if (!sblr::EncodeContextualTextDescriptorV2(
          descriptor, &canonical, &codec_diagnostic)) {
    return Fail(diagnostic,
                codec_diagnostic.code.empty()
                    ? std::string("CTB.TEXT.DESCRIPTOR_INVALID")
                    : std::move(codec_diagnostic.code),
                codec_diagnostic.detail.empty()
                    ? std::string("SBTLTD02 encoding failed")
                    : std::move(codec_diagnostic.detail));
  }
  if (canonical.size() != sblr::kContextualTextDescriptorBytesV2) {
    return Invalid(diagnostic, "SBTLTD02 is not exactly 533 bytes");
  }
  Descriptor decoded;
  if (!sblr::DecodeContextualTextDescriptorV2(
          canonical.data(), canonical.size(), &decoded, &codec_diagnostic)) {
    return Fail(diagnostic,
                codec_diagnostic.code.empty()
                    ? std::string("CTB.TEXT.DESCRIPTOR_INVALID")
                    : std::move(codec_diagnostic.code),
                codec_diagnostic.detail.empty()
                    ? std::string("canonical SBTLTD02 decode failed")
                    : std::move(codec_diagnostic.detail));
  }
  const Sha256 computed =
      sblr::ComputeContextualTextDescriptorEvidenceSha256V2(canonical);
  if (decoded.descriptor_evidence_sha256 != computed ||
      (!descriptor.exact_bytes.empty() &&
       descriptor.exact_bytes != canonical) ||
      (Nonzero(descriptor.descriptor_evidence_sha256) &&
       descriptor.descriptor_evidence_sha256 != computed)) {
    return Invalid(diagnostic,
                   "SBTLTD02 bytes or descriptor evidence are inconsistent");
  }
  if (exact_blob != nullptr) *exact_blob = std::move(canonical);
  if (descriptor_evidence != nullptr) *descriptor_evidence = computed;
  return true;
}

bool CollectComparableColumns(
    std::span<const MgaContextualTextProjectedColumnV2> projected_columns,
    std::vector<const MgaContextualTextProjectedColumnV2*>* comparable,
    MgaContextualTextSidecarSetDiagnosticV2* diagnostic) {
  if (comparable == nullptr) {
    return Invalid(diagnostic, "comparable-column output is null");
  }
  comparable->clear();
  std::set<std::uint32_t> ordinals;
  std::set<Uuid> column_uuids;
  for (const auto& column : projected_columns) {
    if (!Nonzero(column.column_uuid) ||
        !ordinals.insert(column.column_ordinal).second ||
        !column_uuids.insert(column.column_uuid).second) {
      return Invalid(diagnostic,
                     "projected column UUID or ordinal is invalid or duplicated");
    }
    if (!column.comparable_persisted_text) continue;
    RawBytes ignored_blob;
    Sha256 ignored_hash{};
    if (!EncodeExpectedDescriptor(column, &ignored_blob, &ignored_hash,
                                  diagnostic)) {
      return false;
    }
    comparable->push_back(&column);
  }
  std::sort(comparable->begin(), comparable->end(),
            [](const auto* left, const auto* right) {
              return left->column_ordinal < right->column_ordinal;
            });
  if (comparable->size() >
      std::numeric_limits<std::uint32_t>::max()) {
    return Invalid(diagnostic, "contextual sidecar count exceeds u32");
  }
  return true;
}

struct SidecarSealEntry {
  std::uint32_t column_ordinal = 0;
  Uuid column_uuid{};
  Sha256 descriptor_evidence_sha256{};
};

bool ComputeSeal(
    const MgaContextualTextSidecarSetOwnerV2& owner,
    std::uint64_t pre_seal_pair_count,
    std::uint64_t pre_seal_serialized_bytes,
    const RawBytes& pre_seal_serialization,
    std::span<const SidecarSealEntry> sidecars,
    Sha256* seal,
    MgaContextualTextSidecarSetDiagnosticV2* diagnostic) {
  std::uint64_t actual_pre_seal_bytes = 0;
  if (!SizeAsU64(pre_seal_serialization.size(), &actual_pre_seal_bytes) ||
      actual_pre_seal_bytes != pre_seal_serialized_bytes ||
      sidecars.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Invalid(diagnostic,
                   "pre-seal count or serialization length is invalid");
  }
  RawBytes material;
  AppendDomain(&material, kMgaContextualTextSidecarSetSealDomainV2);
  AppendU64(&material, owner.creator_transaction_id);
  AppendU64(&material, owner.event_sequence);
  AppendArray(&material, owner.relation_uuid);
  AppendArray(&material, owner.relation_descriptor_uuid);
  AppendU64(&material, owner.relation_descriptor_generation);
  AppendU64(&material, pre_seal_pair_count);
  AppendU64(&material, pre_seal_serialized_bytes);
  material.insert(material.end(), pre_seal_serialization.begin(),
                  pre_seal_serialization.end());
  AppendU32(&material, static_cast<std::uint32_t>(sidecars.size()));
  for (const auto& sidecar : sidecars) {
    AppendU32(&material, sidecar.column_ordinal);
    AppendArray(&material, sidecar.column_uuid);
    AppendArray(&material, sidecar.descriptor_evidence_sha256);
  }
  return Digest(material, seal, diagnostic);
}

RawBytes ShaBytes(const Sha256& value) {
  return RawBytes(value.begin(), value.end());
}

bool ShaFromBytes(const RawBytes& value, Sha256* out) {
  if (out == nullptr || value.size() != out->size()) return false;
  std::copy(value.begin(), value.end(), out->begin());
  return true;
}

}  // namespace

bool SerializeMgaContextualTextDescriptorFieldVectorV2(
    std::span<const MgaContextualTextDescriptorFieldPairV2> fields,
    MgaContextualTextRawBytesV2* canonical_serialization,
    std::uint64_t* canonical_serialized_bytes,
    MgaContextualTextSidecarSetDiagnosticV2* diagnostic) {
  Clear(diagnostic);
  if (canonical_serialization == nullptr ||
      canonical_serialized_bytes == nullptr) {
    return Invalid(diagnostic, "descriptor field serialization output is null");
  }
  canonical_serialization->clear();
  *canonical_serialized_bytes = 0;
  if (fields.empty()) {
    return Invalid(diagnostic, "descriptor field vector is empty");
  }

  std::uint64_t total = 0;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    const auto& field = fields[index];
    if (field.key_raw_bytes.empty()) {
      return Invalid(diagnostic, "descriptor field key is empty");
    }
    std::uint64_t key_bytes = 0;
    std::uint64_t value_bytes = 0;
    std::uint64_t doubled_key = 0;
    std::uint64_t doubled_value = 0;
    if (!SizeAsU64(field.key_raw_bytes.size(), &key_bytes) ||
        !SizeAsU64(field.value_raw_bytes.size(), &value_bytes) ||
        !CheckedDouble(key_bytes, &doubled_key) ||
        !CheckedDouble(value_bytes, &doubled_value) ||
        (index != 0 && !CheckedAdd(total, 1, &total)) ||
        !CheckedAdd(total, doubled_key, &total) ||
        !CheckedAdd(total, 1, &total) ||
        !CheckedAdd(total, doubled_value, &total)) {
      return Invalid(diagnostic,
                     "descriptor field byte equation overflows u64");
    }
  }
  if (total > std::numeric_limits<std::size_t>::max()) {
    return Invalid(diagnostic,
                   "descriptor field serialization exceeds host size_t");
  }

  canonical_serialization->reserve(static_cast<std::size_t>(total));
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0) canonical_serialization->push_back('|');
    AppendHex(canonical_serialization, fields[index].key_raw_bytes);
    canonical_serialization->push_back('=');
    AppendHex(canonical_serialization, fields[index].value_raw_bytes);
  }
  if (canonical_serialization->size() != total) {
    canonical_serialization->clear();
    return Invalid(diagnostic,
                   "descriptor field byte equation does not match serialization");
  }
  *canonical_serialized_bytes = total;
  return true;
}

bool BuildMgaContextualTextSidecarSetV2(
    const MgaContextualTextSidecarSetOwnerV2& owner,
    std::span<const MgaContextualTextDescriptorFieldPairV2>
        base_descriptor_fields,
    std::span<const MgaContextualTextProjectedColumnV2> projected_columns,
    MgaContextualTextSidecarSetV2* out,
    MgaContextualTextSidecarSetDiagnosticV2* diagnostic) {
  Clear(diagnostic);
  if (out == nullptr) {
    return Invalid(diagnostic, "contextual TEXT sidecar-set output is null");
  }
  if (!ValidOwner(owner, diagnostic) ||
      !ValidateBaseFields(base_descriptor_fields, diagnostic)) {
    return false;
  }
  std::vector<const MgaContextualTextProjectedColumnV2*> comparable;
  if (!CollectComparableColumns(projected_columns, &comparable, diagnostic)) {
    return false;
  }
  if (base_descriptor_fields.size() ==
          std::numeric_limits<std::size_t>::max() ||
      comparable.size() >
      (std::numeric_limits<std::size_t>::max() -
       base_descriptor_fields.size() - 1) /
          2) {
    return Invalid(diagnostic, "descriptor field count exceeds size_t");
  }

  MgaContextualTextSidecarSetV2 built;
  built.owner = owner;
  built.descriptor_fields.assign(base_descriptor_fields.begin(),
                                 base_descriptor_fields.end());
  built.descriptor_fields.reserve(base_descriptor_fields.size() +
                                  comparable.size() * 2 + 1);
  std::vector<SidecarSealEntry> sidecar_seal_entries;
  sidecar_seal_entries.reserve(comparable.size());
  for (const auto* column : comparable) {
    RawBytes blob;
    Sha256 evidence{};
    if (!EncodeExpectedDescriptor(*column, &blob, &evidence, diagnostic)) {
      return false;
    }
    built.descriptor_fields.push_back(
        {Raw(BlobKey(column->column_ordinal)), std::move(blob)});
    built.descriptor_fields.push_back(
        {Raw(HashKey(column->column_ordinal)), ShaBytes(evidence)});
    sidecar_seal_entries.push_back(
        {column->column_ordinal, column->column_uuid, evidence});
  }

  std::uint64_t pre_seal_pair_count = 0;
  if (!SizeAsU64(built.descriptor_fields.size(), &pre_seal_pair_count)) {
    return Invalid(diagnostic, "pre-seal descriptor field count exceeds u64");
  }
  RawBytes pre_seal_serialization;
  std::uint64_t pre_seal_serialized_bytes = 0;
  if (!SerializeMgaContextualTextDescriptorFieldVectorV2(
          built.descriptor_fields, &pre_seal_serialization,
          &pre_seal_serialized_bytes, diagnostic)) {
    return false;
  }
  if (!ComputeSeal(owner, pre_seal_pair_count, pre_seal_serialized_bytes,
                   pre_seal_serialization, sidecar_seal_entries,
                   &built.seal_sha256, diagnostic)) {
    return false;
  }
  built.descriptor_fields.push_back(
      {Raw(kMgaContextualTextSidecarSetSealKeyV2),
       ShaBytes(built.seal_sha256)});
  if (!CheckedAdd(pre_seal_pair_count, 1, &built.descriptor_field_count) ||
      !CheckedAdd(pre_seal_serialized_bytes, 172,
                  &built.descriptor_field_bytes)) {
    return Invalid(diagnostic,
                   "complete descriptor field count or bytes overflows u64");
  }
  built.contextual_sidecar_count =
      static_cast<std::uint32_t>(comparable.size());

  RawBytes complete_serialization;
  std::uint64_t complete_serialized_bytes = 0;
  if (!SerializeMgaContextualTextDescriptorFieldVectorV2(
          built.descriptor_fields, &complete_serialization,
          &complete_serialized_bytes, diagnostic) ||
      complete_serialized_bytes != built.descriptor_field_bytes) {
    return Invalid(diagnostic,
                   "complete descriptor field byte equation is inconsistent");
  }
  *out = std::move(built);
  return true;
}

bool ValidateMgaContextualTextSidecarSetV2(
    const MgaContextualTextSidecarSetOwnerV2& expected_owner,
    std::span<const MgaContextualTextDescriptorFieldPairV2>
        expected_base_descriptor_fields,
    std::span<const MgaContextualTextProjectedColumnV2>
        expected_projected_columns,
    const MgaContextualTextSidecarSetV2& candidate,
    MgaContextualTextSidecarSetDiagnosticV2* diagnostic) {
  Clear(diagnostic);
  if (!ValidOwner(expected_owner, diagnostic) ||
      !ValidateBaseFields(expected_base_descriptor_fields, diagnostic)) {
    return false;
  }
  if (candidate.owner != expected_owner ||
      !ValidOwner(candidate.owner, diagnostic)) {
    return Invalid(diagnostic,
                   "selected contextual sidecar-set owner does not match");
  }
  std::vector<const MgaContextualTextProjectedColumnV2*> comparable;
  if (!CollectComparableColumns(expected_projected_columns, &comparable,
                                diagnostic)) {
    return false;
  }
  if (expected_base_descriptor_fields.size() ==
          std::numeric_limits<std::size_t>::max() ||
      comparable.size() >
      (std::numeric_limits<std::size_t>::max() -
       expected_base_descriptor_fields.size() - 1) /
          2) {
    return Invalid(diagnostic, "expected descriptor field count exceeds size_t");
  }
  const std::size_t expected_field_count =
      expected_base_descriptor_fields.size() + comparable.size() * 2 + 1;
  if (candidate.descriptor_fields.size() != expected_field_count ||
      candidate.contextual_sidecar_count != comparable.size()) {
    return Invalid(diagnostic,
                   "contextual sidecar or descriptor field count is incorrect");
  }
  std::uint64_t actual_field_count = 0;
  if (!SizeAsU64(candidate.descriptor_fields.size(), &actual_field_count) ||
      candidate.descriptor_field_count != actual_field_count) {
    return Invalid(diagnostic,
                   "descriptor_field_count does not include the final seal pair");
  }

  std::set<RawBytes> unique_keys;
  for (const auto& field : candidate.descriptor_fields) {
    if (field.key_raw_bytes.empty() ||
        !unique_keys.insert(field.key_raw_bytes).second) {
      return Invalid(diagnostic,
                     "descriptor field key is empty or duplicated");
    }
  }
  for (std::size_t index = 0;
       index < expected_base_descriptor_fields.size(); ++index) {
    if (candidate.descriptor_fields[index] !=
            expected_base_descriptor_fields[index] ||
        ParseKey(candidate.descriptor_fields[index].key_raw_bytes).kind !=
            KeyKind::kBase) {
      return Invalid(diagnostic,
                     "base descriptor field order or bytes changed");
    }
  }

  std::vector<SidecarSealEntry> sidecar_seal_entries;
  sidecar_seal_entries.reserve(comparable.size());
  for (std::size_t sidecar_index = 0;
       sidecar_index < comparable.size(); ++sidecar_index) {
    const auto& column = *comparable[sidecar_index];
    const std::size_t blob_index =
        expected_base_descriptor_fields.size() + sidecar_index * 2;
    const std::size_t hash_index = blob_index + 1;
    const auto& blob_pair = candidate.descriptor_fields[blob_index];
    const auto& hash_pair = candidate.descriptor_fields[hash_index];
    const ParsedKey blob_key = ParseKey(blob_pair.key_raw_bytes);
    const ParsedKey hash_key = ParseKey(hash_pair.key_raw_bytes);
    if (blob_key.kind != KeyKind::kBlob ||
        hash_key.kind != KeyKind::kHash ||
        blob_key.ordinal != column.column_ordinal ||
        hash_key.ordinal != column.column_ordinal ||
        blob_pair.key_raw_bytes != Raw(BlobKey(column.column_ordinal)) ||
        hash_pair.key_raw_bytes != Raw(HashKey(column.column_ordinal)) ||
        blob_pair.value_raw_bytes.size() !=
            sblr::kContextualTextDescriptorBytesV2 ||
        hash_pair.value_raw_bytes.size() != Sha256{}.size()) {
      return Invalid(
          diagnostic,
          "contextual sidecar key grammar, order, adjacency, or extent is invalid");
    }

    Descriptor decoded;
    sblr::ContextualTextCodecDiagnosticV2 codec_diagnostic;
    if (!sblr::DecodeContextualTextDescriptorV2(
            blob_pair.value_raw_bytes.data(), blob_pair.value_raw_bytes.size(),
            &decoded, &codec_diagnostic)) {
      return Fail(diagnostic,
                  codec_diagnostic.code.empty()
                      ? std::string("CTB.TEXT.DESCRIPTOR_INVALID")
                      : std::move(codec_diagnostic.code),
                  codec_diagnostic.detail.empty()
                      ? std::string("stored SBTLTD02 decode failed")
                      : std::move(codec_diagnostic.detail));
    }
    RawBytes expected_blob;
    Sha256 expected_evidence{};
    if (!EncodeExpectedDescriptor(column, &expected_blob, &expected_evidence,
                                  diagnostic)) {
      return false;
    }
    Sha256 adjacent_evidence{};
    if (!ShaFromBytes(hash_pair.value_raw_bytes, &adjacent_evidence) ||
        blob_pair.value_raw_bytes != expected_blob ||
        adjacent_evidence != expected_evidence ||
        decoded.descriptor_evidence_sha256 != adjacent_evidence ||
        sblr::ComputeContextualTextDescriptorEvidenceSha256V2(
            blob_pair.value_raw_bytes) != adjacent_evidence) {
      return Invalid(
          diagnostic,
          "contextual sidecar blob, adjacent hash, or projected authority differs");
    }
    sidecar_seal_entries.push_back(
        {column.column_ordinal, column.column_uuid, adjacent_evidence});
  }

  const auto& final_pair = candidate.descriptor_fields.back();
  if (ParseKey(final_pair.key_raw_bytes).kind != KeyKind::kFinalSeal ||
      final_pair.key_raw_bytes != Raw(kMgaContextualTextSidecarSetSealKeyV2) ||
      final_pair.value_raw_bytes.size() != Sha256{}.size() ||
      final_pair.value_raw_bytes != ShaBytes(candidate.seal_sha256)) {
    return Invalid(diagnostic,
                   "final contextual sidecar-set seal pair is not exact");
  }

  const std::span<const FieldPair> complete_fields(candidate.descriptor_fields);
  const auto pre_seal_fields = complete_fields.first(complete_fields.size() - 1);
  RawBytes pre_seal_serialization;
  std::uint64_t pre_seal_serialized_bytes = 0;
  if (!SerializeMgaContextualTextDescriptorFieldVectorV2(
          pre_seal_fields, &pre_seal_serialization,
          &pre_seal_serialized_bytes, diagnostic)) {
    return false;
  }
  std::uint64_t pre_seal_pair_count = 0;
  std::uint64_t expected_complete_count = 0;
  std::uint64_t expected_complete_bytes = 0;
  if (!SizeAsU64(pre_seal_fields.size(), &pre_seal_pair_count) ||
      !CheckedAdd(pre_seal_pair_count, 1, &expected_complete_count) ||
      !CheckedAdd(pre_seal_serialized_bytes, 172,
                  &expected_complete_bytes) ||
      candidate.descriptor_field_count != expected_complete_count ||
      candidate.descriptor_field_bytes != expected_complete_bytes) {
    return Invalid(
        diagnostic,
        "descriptor field count or bytes violates pre-seal plus-one/plus-172");
  }

  RawBytes complete_serialization;
  std::uint64_t complete_serialized_bytes = 0;
  if (!SerializeMgaContextualTextDescriptorFieldVectorV2(
          complete_fields, &complete_serialization,
          &complete_serialized_bytes, diagnostic) ||
      complete_serialized_bytes != candidate.descriptor_field_bytes) {
    return Invalid(diagnostic,
                   "descriptor_field_bytes differs from canonical serialization");
  }

  Sha256 computed_seal{};
  if (!ComputeSeal(candidate.owner, pre_seal_pair_count,
                   pre_seal_serialized_bytes, pre_seal_serialization,
                   sidecar_seal_entries, &computed_seal, diagnostic)) {
    return false;
  }
  if (computed_seal != candidate.seal_sha256 ||
      final_pair.value_raw_bytes != ShaBytes(computed_seal)) {
    return Invalid(diagnostic,
                   "contextual sidecar-set final seal does not match material");
  }
  return true;
}

bool LookupMgaContextualTextSidecarV2(
    const MgaContextualTextSidecarSetOwnerV2& expected_owner,
    std::span<const MgaContextualTextDescriptorFieldPairV2>
        expected_base_descriptor_fields,
    std::span<const MgaContextualTextProjectedColumnV2>
        expected_projected_columns,
    const MgaContextualTextSidecarSetV2& candidate,
    std::uint32_t column_ordinal,
    const MgaContextualTextUuidV2& column_uuid,
    MgaContextualTextSidecarLookupResultV2* out,
    MgaContextualTextSidecarSetDiagnosticV2* diagnostic) {
  Clear(diagnostic);
  if (out == nullptr) {
    return Invalid(diagnostic, "contextual sidecar lookup output is null");
  }
  if (!ValidateMgaContextualTextSidecarSetV2(
          expected_owner, expected_base_descriptor_fields,
          expected_projected_columns, candidate, diagnostic)) {
    return false;
  }

  const MgaContextualTextProjectedColumnV2* expected_column = nullptr;
  std::size_t projected_matches = 0;
  for (const auto& column : expected_projected_columns) {
    if (column.column_ordinal == column_ordinal &&
        column.column_uuid == column_uuid) {
      ++projected_matches;
      expected_column = &column;
    }
  }
  if (projected_matches != 1 || expected_column == nullptr ||
      !expected_column->comparable_persisted_text) {
    return Invalid(diagnostic,
                   "requested projected column has no contextual sidecar");
  }

  const RawBytes expected_blob_key = Raw(BlobKey(column_ordinal));
  const RawBytes expected_hash_key = Raw(HashKey(column_ordinal));
  std::size_t blob_matches = 0;
  std::size_t hash_matches = 0;
  std::size_t blob_index = 0;
  std::size_t hash_index = 0;
  for (std::size_t index = 0; index < candidate.descriptor_fields.size();
       ++index) {
    const auto& field = candidate.descriptor_fields[index];
    if (field.key_raw_bytes == expected_blob_key) {
      ++blob_matches;
      blob_index = index;
    }
    if (field.key_raw_bytes == expected_hash_key) {
      ++hash_matches;
      hash_index = index;
    }
  }
  if (blob_matches != 1 || hash_matches != 1 ||
      hash_index != blob_index + 1) {
    return Invalid(diagnostic,
                   "full-vector lookup did not find one adjacent blob/hash pair");
  }

  const auto& blob = candidate.descriptor_fields[blob_index].value_raw_bytes;
  const auto& hash = candidate.descriptor_fields[hash_index].value_raw_bytes;
  MgaContextualTextSidecarLookupResultV2 found;
  sblr::ContextualTextCodecDiagnosticV2 codec_diagnostic;
  if (!sblr::DecodeContextualTextDescriptorV2(
          blob.data(), blob.size(), &found.descriptor, &codec_diagnostic) ||
      !ShaFromBytes(hash, &found.descriptor_evidence_sha256)) {
    return Fail(diagnostic,
                codec_diagnostic.code.empty()
                    ? std::string("CTB.TEXT.DESCRIPTOR_INVALID")
                    : std::move(codec_diagnostic.code),
                codec_diagnostic.detail.empty()
                    ? std::string("contextual sidecar lookup decode failed")
                    : std::move(codec_diagnostic.detail));
  }
  found.exact_blob = blob;
  *out = std::move(found);
  return true;
}

}  // namespace scratchbird::engine::internal_api
