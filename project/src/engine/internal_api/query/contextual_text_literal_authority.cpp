// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/contextual_text_literal_authority.hpp"

#include "api_diagnostics.hpp"
#include "catalog/name_resolution_api.hpp"
#include "hash_digest.hpp"
#include "query/contextual_text_target_authority_resolver_v2.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <tuple>
#include <type_traits>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace sblr = scratchbird::engine::sblr;

EngineApiDiagnostic Diagnostic(std::string code,
                               std::string key,
                               std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail), true);
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {},
                                 false);
}

bool Nonzero(const sblr::ContextualTextUuidV2& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

bool Nonzero(const sblr::ContextualTextSha256V2& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

bool ExactUuid(std::string_view text) {
  if (text.empty()) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  return parsed.ok() && !scratchbird::core::uuid::IsNilUuid(parsed.value) &&
         scratchbird::core::uuid::UuidToString(parsed.value) == text;
}

bool ToWireUuid(std::string_view text, sblr::ContextualTextUuidV2* output) {
  if (output == nullptr || !ExactUuid(text)) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(),
            output->begin());
  return true;
}

std::string Hex(const sblr::ContextualTextUuidV2& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(32);
  for (const auto byte : value) {
    result.push_back(kHex[byte >> 4]);
    result.push_back(kHex[byte & 0x0f]);
  }
  return result;
}

std::string UuidText(const sblr::ContextualTextUuidV2& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(36);
  for (std::size_t index = 0; index != value.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      result.push_back('-');
    }
    result.push_back(kHex[value[index] >> 4]);
    result.push_back(kHex[value[index] & 0x0f]);
  }
  return result;
}

std::optional<std::string> ExactEncodedDescriptorField(
    const std::string_view descriptor,
    const std::string_view requested_key) {
  std::optional<std::string> result;
  std::size_t start = 0;
  while (start <= descriptor.size()) {
    const auto end = descriptor.find(';', start);
    const auto field = descriptor.substr(
        start, end == std::string_view::npos ? std::string_view::npos
                                             : end - start);
    const auto equals = field.find('=');
    if (field.empty() || equals == std::string_view::npos || equals == 0 ||
        equals + 1 == field.size()) {
      return std::nullopt;
    }
    if (field.substr(0, equals) == requested_key) {
      if (result.has_value()) return std::nullopt;
      result = std::string(field.substr(equals + 1));
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return result;
}

std::array<std::string, 17> ExpectedLiteralDescriptorFields(
    const sblr::ContextualTextLiteralProfileV2& profile) {
  const std::string width =
      profile.target_character_limit ==
              std::numeric_limits<std::uint64_t>::max()
          ? "-"
          : std::to_string(profile.target_character_limit);
  return {UuidText(profile.descriptor_uuid),
          std::to_string(profile.descriptor_generation),
          UuidText(profile.type_uuid),
          std::to_string(profile.type_generation),
          sblr::kContextualTextCodecIdentifierV2,
          std::to_string(profile.codec_version),
          std::to_string(profile.codec_generation),
          "0",
          UuidText(profile.collation_uuid),
          "-",
          width,
          "-",
          "-",
          UuidText(profile.statement_receipt_uuid),
          UuidText(profile.catalog_snapshot_uuid),
          std::to_string(profile.catalog_generation),
          std::to_string(profile.datatype_registry_generation)};
}

std::string JoinLiteralDescriptorFields(
    const std::array<std::string, 17>& fields) {
  std::size_t bytes = fields.size() - 1;
  for (const auto& field : fields) bytes += field.size();
  std::string result;
  result.reserve(bytes);
  for (std::size_t index = 0; index != fields.size(); ++index) {
    if (index != 0) result.push_back('|');
    result.append(fields[index]);
  }
  return result;
}

void AppendRuntimeDescriptorField(std::string* descriptor,
                                  std::string_view key,
                                  std::string_view value) {
  if (!descriptor->empty()) descriptor->push_back(';');
  descriptor->append(key);
  descriptor->push_back('=');
  descriptor->append(value);
}

std::string RuntimeEncodedDescriptor(
    const std::array<std::string, 17>& fields,
    const sblr::ContextualTextLiteralProfileV2& profile,
    std::string_view exact_relational_descriptor) {
  std::string result;
  AppendRuntimeDescriptorField(&result, "canonical", "text");
  AppendRuntimeDescriptorField(&result, "datatype_descriptor_uuid", fields[0]);
  AppendRuntimeDescriptorField(&result, "datatype_descriptor_generation",
                               fields[1]);
  AppendRuntimeDescriptorField(&result, "type_uuid", fields[2]);
  AppendRuntimeDescriptorField(&result, "type_generation", fields[3]);
  AppendRuntimeDescriptorField(&result, "codec_uuid",
                               UuidText(profile.codec_uuid));
  AppendRuntimeDescriptorField(&result, "codec_id", fields[4]);
  AppendRuntimeDescriptorField(&result, "codec_version", fields[5]);
  AppendRuntimeDescriptorField(&result, "codec_generation", fields[6]);
  AppendRuntimeDescriptorField(&result, "null_encoding", "1");
  AppendRuntimeDescriptorField(&result, "nullability",
                               fields[7] == "1" ? "nullable" : "non_null");
  AppendRuntimeDescriptorField(&result, "charset_uuid",
                               UuidText(profile.charset_uuid));
  AppendRuntimeDescriptorField(&result, "charset_generation",
                               std::to_string(profile.charset_generation));
  AppendRuntimeDescriptorField(&result, "collation_uuid", fields[8]);
  AppendRuntimeDescriptorField(&result, "collation_generation",
                               std::to_string(profile.collation_generation));
  AppendRuntimeDescriptorField(&result, "resource_epoch",
                               std::to_string(profile.resource_epoch));
  AppendRuntimeDescriptorField(&result, "timezone_profile_id", fields[9]);
  if (fields[10] != "-") {
    AppendRuntimeDescriptorField(&result, "width", fields[10]);
  }
  AppendRuntimeDescriptorField(&result, "precision", fields[11]);
  AppendRuntimeDescriptorField(&result, "scale", fields[12]);
  AppendRuntimeDescriptorField(&result, "statement_receipt_uuid", fields[13]);
  AppendRuntimeDescriptorField(&result, "datatype_catalog_snapshot_uuid",
                               fields[14]);
  AppendRuntimeDescriptorField(&result, "datatype_catalog_generation",
                               fields[15]);
  AppendRuntimeDescriptorField(&result, "datatype_registry_generation",
                               fields[16]);
  AppendRuntimeDescriptorField(&result, "relational_descriptor_v2",
                               exact_relational_descriptor);
  return result;
}

bool SameTextSeed(
    const scratchbird::core::datatypes::DatatypeTextSeedAuthority& left,
    const scratchbird::core::datatypes::DatatypeTextSeedAuthority& right) {
  return left.active == right.active &&
         left.seed_pack_name == right.seed_pack_name &&
         left.seed_pack_version == right.seed_pack_version &&
         left.charset_name == right.charset_name &&
         left.collation_name == right.collation_name &&
         left.collation_case_insensitive ==
             right.collation_case_insensitive &&
         left.collation_accent_insensitive ==
             right.collation_accent_insensitive;
}

bool SameResolvedResourceDescriptor(
    const EngineResolvedResourceDescriptor& left,
    const EngineResolvedResourceDescriptor& right) {
  return left.present == right.present &&
         left.resource_family == right.resource_family &&
         left.canonical_name == right.canonical_name &&
         left.resource_uuid.canonical == right.resource_uuid.canonical &&
         left.parent_resource_uuid.canonical ==
             right.parent_resource_uuid.canonical &&
         left.parent_canonical_name == right.parent_canonical_name &&
         left.default_collation_uuid.canonical ==
             right.default_collation_uuid.canonical &&
         left.default_collation_name == right.default_collation_name &&
         left.seed_pack_name == right.seed_pack_name &&
         left.seed_pack_version == right.seed_pack_version &&
         left.resource_epoch == right.resource_epoch &&
         left.family_epoch == right.family_epoch &&
         left.family_version == right.family_version &&
         left.min_bytes == right.min_bytes &&
         left.max_bytes == right.max_bytes &&
         left.variable_width == right.variable_width &&
         left.default_for_parent == right.default_for_parent &&
         left.case_insensitive == right.case_insensitive &&
         left.accent_insensitive == right.accent_insensitive;
}

bool ValidComparisonResources(
    const EngineContextualTextComparisonResourceSnapshotV2& resources,
    const sblr::ContextualTextLiteralDemandV2& demand,
    const sblr::ContextualTextLiteralProfileV2& profile,
    const EngineResolvedContextualTextTargetV2& target) {
  return resources.charset_uuid == profile.charset_uuid &&
         resources.charset_generation == profile.charset_generation &&
         !resources.charset_name.empty() &&
         resources.charset_uuid_canonical == UuidText(profile.charset_uuid) &&
         resources.charset_resource_epoch == profile.resource_epoch &&
         resources.charset_family_epoch == profile.charset_generation &&
         resources.collation_uuid == profile.collation_uuid &&
         resources.collation_generation == profile.collation_generation &&
         !resources.collation_name.empty() &&
         resources.collation_uuid_canonical ==
             UuidText(profile.collation_uuid) &&
         resources.collation_resource_epoch == profile.resource_epoch &&
         resources.collation_family_epoch == profile.collation_generation &&
         resources.charset_resource.present &&
         resources.charset_resource.resource_family == "charset" &&
         resources.charset_resource.resource_uuid.canonical ==
             resources.charset_uuid_canonical &&
         resources.charset_resource.canonical_name == resources.charset_name &&
         resources.charset_resource.resource_epoch ==
             resources.charset_resource_epoch &&
         resources.charset_resource.family_epoch ==
             resources.charset_family_epoch &&
         resources.collation_resource.present &&
         resources.collation_resource.resource_family == "collation" &&
         resources.collation_resource.resource_uuid.canonical ==
             resources.collation_uuid_canonical &&
         resources.collation_resource.canonical_name ==
             resources.collation_name &&
         resources.collation_resource.parent_resource_uuid.canonical ==
             resources.charset_uuid_canonical &&
         resources.collation_resource.parent_canonical_name ==
             resources.charset_name &&
         resources.collation_resource.resource_epoch ==
             resources.collation_resource_epoch &&
         resources.collation_resource.family_epoch ==
             resources.collation_family_epoch &&
         resources.text_seed.active &&
         !resources.text_seed.seed_pack_name.empty() &&
         !resources.text_seed.seed_pack_version.empty() &&
         resources.text_seed.charset_name == resources.charset_name &&
         resources.text_seed.collation_name == resources.collation_name &&
         resources.text_seed.seed_pack_name ==
             resources.collation_resource.seed_pack_name &&
         resources.text_seed.seed_pack_version ==
             resources.collation_resource.seed_pack_version &&
         resources.text_seed.collation_case_insensitive ==
             resources.collation_resource.case_insensitive &&
         resources.text_seed.collation_accent_insensitive ==
             resources.collation_resource.accent_insensitive &&
         resources.target_projection_sha256 ==
             profile.target_projection_sha256 &&
         resources.target_projection_sha256 ==
             sblr::ComputeContextualTextTargetProjectionSha256V2(
                 demand, profile.source_occurrence_uuid,
                 profile.source_generation,
                 resources.exact_public_relation_projection_v3) &&
         resources.descriptor_evidence_sha256 ==
             profile.descriptor_evidence_sha256 &&
         resources.descriptor_evidence_sha256 ==
             sblr::ComputeContextualTextDescriptorEvidenceSha256V2(
                 resources.exact_sbtltd02) &&
         resources.exact_public_relation_projection_v3 ==
             target.exact_public_relation_projection_v3 &&
         resources.exact_sbtltd02 == target.exact_sbtltd02;
}

bool SameComparisonResources(
    const EngineContextualTextComparisonResourceSnapshotV2& left,
    const EngineContextualTextComparisonResourceSnapshotV2& right) {
  return left.charset_uuid == right.charset_uuid &&
         left.charset_generation == right.charset_generation &&
         left.charset_name == right.charset_name &&
         left.charset_uuid_canonical == right.charset_uuid_canonical &&
         left.charset_resource_epoch == right.charset_resource_epoch &&
         left.charset_family_epoch == right.charset_family_epoch &&
         left.collation_uuid == right.collation_uuid &&
         left.collation_generation == right.collation_generation &&
         left.collation_name == right.collation_name &&
         left.collation_uuid_canonical == right.collation_uuid_canonical &&
         left.collation_resource_epoch == right.collation_resource_epoch &&
         left.collation_family_epoch == right.collation_family_epoch &&
         SameTextSeed(left.text_seed, right.text_seed) &&
         SameResolvedResourceDescriptor(left.charset_resource,
                                        right.charset_resource) &&
         SameResolvedResourceDescriptor(left.collation_resource,
                                        right.collation_resource) &&
         left.target_projection_sha256 == right.target_projection_sha256 &&
         left.descriptor_evidence_sha256 ==
             right.descriptor_evidence_sha256 &&
         left.exact_public_relation_projection_v3 ==
             right.exact_public_relation_projection_v3 &&
         left.exact_sbtltd02 == right.exact_sbtltd02;
}

bool ResolveLiveComparisonResources(
    const EngineRequestContext& context,
    const sblr::ContextualTextLiteralDemandV2& demand,
    const sblr::ContextualTextLiteralProfileV2& profile,
    const EngineResolvedContextualTextTargetV2& target,
    EngineContextualTextComparisonResourceSnapshotV2* resources,
    EngineApiDiagnostic* diagnostic) {
  if (resources == nullptr) return false;
  sblr::ContextualTextDescriptorV2 descriptor;
  sblr::ContextualTextCodecDiagnosticV2 codec_diagnostic;
  if (!sblr::DecodeContextualTextDescriptorV2(
          target.exact_sbtltd02.data(), target.exact_sbtltd02.size(),
          &descriptor, &codec_diagnostic) ||
      descriptor.descriptor_uuid != profile.descriptor_uuid ||
      descriptor.descriptor_generation != profile.descriptor_generation ||
      descriptor.type_uuid != profile.type_uuid ||
      descriptor.type_generation != profile.type_generation ||
      descriptor.codec_uuid != profile.codec_uuid ||
      descriptor.codec_version != profile.codec_version ||
      descriptor.codec_generation != profile.codec_generation ||
      descriptor.charset_uuid != profile.charset_uuid ||
      descriptor.charset_generation != profile.charset_generation ||
      descriptor.collation_uuid != profile.collation_uuid ||
      descriptor.collation_generation != profile.collation_generation ||
      descriptor.resource_epoch != profile.resource_epoch ||
      descriptor.descriptor_evidence_sha256 !=
          profile.descriptor_evidence_sha256) {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "CTB.TEXT.DESCRIPTOR_INVALID",
          "engine.contextual_text_literal.runtime_descriptor_mismatch");
    }
    return false;
  }

  const EngineUuid charset_uuid{UuidText(profile.charset_uuid)};
  const auto charset = LookupEngineResourceDescriptorByUuid(
      context, charset_uuid, "charset");
  if (!charset.ok || !charset.resource_descriptor.present ||
      charset.resource_descriptor.resource_uuid.canonical !=
          charset_uuid.canonical ||
      charset.resource_descriptor.resource_epoch != profile.resource_epoch ||
      charset.resource_descriptor.family_epoch != profile.charset_generation ||
      charset.resource_descriptor.canonical_name.empty()) {
    if (diagnostic != nullptr) {
      *diagnostic = charset.ok
                        ? Diagnostic(
                              "CTB.TEXT.RESOURCE_EPOCH_MISMATCH",
                              "engine.contextual_text_literal.charset_stale")
                        : charset.diagnostic;
    }
    return false;
  }
  const EngineUuid collation_uuid{UuidText(profile.collation_uuid)};
  const auto collation = LookupEngineResourceDescriptorByUuid(
      context, collation_uuid, "collation");
  if (!collation.ok || !collation.resource_descriptor.present ||
      collation.resource_descriptor.resource_uuid.canonical !=
          collation_uuid.canonical ||
      collation.resource_descriptor.parent_resource_uuid.canonical !=
          charset_uuid.canonical ||
      collation.resource_descriptor.parent_canonical_name !=
          charset.resource_descriptor.canonical_name ||
      collation.resource_descriptor.resource_epoch != profile.resource_epoch ||
      collation.resource_descriptor.family_epoch !=
          profile.collation_generation ||
      collation.resource_descriptor.seed_pack_name.empty() ||
      collation.resource_descriptor.seed_pack_version.empty() ||
      collation.resource_descriptor.canonical_name.empty()) {
    if (diagnostic != nullptr) {
      *diagnostic = collation.ok
                        ? Diagnostic(
                              "CTB.TEXT.RESOURCE_EPOCH_MISMATCH",
                              "engine.contextual_text_literal.collation_stale")
                        : collation.diagnostic;
    }
    return false;
  }

  EngineContextualTextComparisonResourceSnapshotV2 resolved;
  resolved.charset_uuid = profile.charset_uuid;
  resolved.charset_generation = profile.charset_generation;
  resolved.charset_name = charset.resource_descriptor.canonical_name;
  resolved.charset_uuid_canonical = charset_uuid.canonical;
  resolved.charset_resource_epoch =
      charset.resource_descriptor.resource_epoch;
  resolved.charset_family_epoch = charset.resource_descriptor.family_epoch;
  resolved.collation_uuid = profile.collation_uuid;
  resolved.collation_generation = profile.collation_generation;
  resolved.collation_name = collation.resource_descriptor.canonical_name;
  resolved.collation_uuid_canonical = collation_uuid.canonical;
  resolved.collation_resource_epoch =
      collation.resource_descriptor.resource_epoch;
  resolved.collation_family_epoch =
      collation.resource_descriptor.family_epoch;
  resolved.text_seed.active = true;
  resolved.text_seed.seed_pack_name =
      collation.resource_descriptor.seed_pack_name;
  resolved.text_seed.seed_pack_version =
      collation.resource_descriptor.seed_pack_version;
  resolved.text_seed.charset_name =
      collation.resource_descriptor.parent_canonical_name;
  resolved.text_seed.collation_name =
      collation.resource_descriptor.canonical_name;
  resolved.text_seed.collation_case_insensitive =
      collation.resource_descriptor.case_insensitive;
  resolved.text_seed.collation_accent_insensitive =
      collation.resource_descriptor.accent_insensitive;
  resolved.charset_resource = charset.resource_descriptor;
  resolved.collation_resource = collation.resource_descriptor;
  resolved.target_projection_sha256 = profile.target_projection_sha256;
  resolved.descriptor_evidence_sha256 =
      profile.descriptor_evidence_sha256;
  resolved.exact_public_relation_projection_v3 =
      target.exact_public_relation_projection_v3;
  resolved.exact_sbtltd02 = target.exact_sbtltd02;
  if (!ValidComparisonResources(resolved, demand, profile, target)) {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "CTB.TEXT.RESOURCE_EPOCH_MISMATCH",
          "engine.contextual_text_literal.comparison_resource_incomplete");
    }
    return false;
  }
  *resources = std::move(resolved);
  if (diagnostic != nullptr) *diagnostic = OkDiagnostic();
  return true;
}

bool ResolveComparisonResources(
    const EngineRequestContext& context,
    const EngineContextualTextTargetAuthorityResolverV2& resolver,
    const sblr::ContextualTextLiteralDemandV2& demand,
    const sblr::ContextualTextLiteralProfileV2& profile,
    const EngineResolvedContextualTextTargetV2& target,
    EngineContextualTextComparisonResourceSnapshotV2* resources,
    EngineApiDiagnostic* diagnostic) {
  EngineContextualTextComparisonResourceSnapshotV2 retained;
  if (resolver.CopyComparisonResourceSnapshot(context, demand, target, profile,
                                              &retained)) {
    if (!ValidComparisonResources(retained, demand, profile, target)) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "CTB.TEXT.RESOURCE_EPOCH_MISMATCH",
            "engine.contextual_text_literal.resolver_resource_invalid");
      }
      return false;
    }
    *resources = std::move(retained);
    if (diagnostic != nullptr) *diagnostic = OkDiagnostic();
    return true;
  }
  return ResolveLiveComparisonResources(context, demand, profile, target,
                                        resources, diagnostic);
}

bool ResolveProjectedTargetDescriptor(
    const sblr::ContextualTextLiteralDemandV2& demand,
    const sblr::ContextualTextLiteralProfileV2& profile,
    const EngineResolvedContextualTextTargetV2& target,
    const std::array<std::string, 17>& exact_target_descriptor_fields,
    EngineDescriptor* descriptor,
    EngineApiDiagnostic* diagnostic) {
  if (descriptor == nullptr) return false;
  EnginePublicRelationProjectionV3 projection;
  if (!DecodeEnginePublicRelationProjectionV3(
          target.exact_public_relation_projection_v3, &projection,
          diagnostic) ||
      projection.relation_descriptor_uuid !=
          demand.relation_descriptor_uuid ||
      projection.relation_uuid != demand.relation_uuid ||
      projection.relation_descriptor_generation !=
          demand.relation_descriptor_generation ||
      projection.resource_epoch != profile.resource_epoch ||
      projection.catalog_snapshot_uuid != profile.catalog_snapshot_uuid ||
      projection.catalog_generation != profile.catalog_generation ||
      projection.registry_generation !=
          profile.datatype_registry_generation) {
    if (diagnostic != nullptr && !diagnostic->error) {
      *diagnostic = Diagnostic(
          "CTB.TEXT.DESCRIPTOR_INVALID",
          "engine.contextual_text_literal.target_projection_context_mismatch");
    }
    return false;
  }
  const EnginePublicRelationProjectionColumnV3* selected = nullptr;
  for (const auto& column : projection.columns) {
    if (column.column_uuid != demand.column_uuid ||
        column.ordinal != demand.column_ordinal) {
      continue;
    }
    if (selected != nullptr) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "CTB.TEXT.DESCRIPTOR_INVALID",
            "engine.contextual_text_literal.target_projection_ambiguous");
      }
      return false;
    }
    selected = &column;
  }
  const bool character_limit_matches =
      profile.target_character_limit ==
              std::numeric_limits<std::uint64_t>::max()
          ? selected != nullptr && selected->character_length == 0
          : selected != nullptr &&
                profile.target_character_limit <=
                    std::numeric_limits<std::uint32_t>::max() &&
                selected->character_length == profile.target_character_limit;
  const auto embedded_datatype_descriptor_uuid =
      selected == nullptr
          ? std::optional<std::string>{}
          : ExactEncodedDescriptorField(
                selected->encoded_type_descriptor,
                "datatype_descriptor_uuid");
  if (selected == nullptr || !selected->identity_present ||
      (exact_target_descriptor_fields[7] != "0" &&
       exact_target_descriptor_fields[7] != "1") ||
      (((selected->attributes & 0x01u) != 0) !=
       (exact_target_descriptor_fields[7] == "1")) ||
      !embedded_datatype_descriptor_uuid.has_value() ||
      *embedded_datatype_descriptor_uuid != UuidText(profile.descriptor_uuid) ||
      selected->descriptor_generation != profile.descriptor_generation ||
      selected->type_uuid != profile.type_uuid ||
      selected->type_generation != profile.type_generation ||
      selected->codec_id != sblr::kContextualTextCodecIdentifierV2 ||
      selected->codec_version != profile.codec_version ||
      selected->codec_generation != profile.codec_generation ||
      selected->canonical_value_width != 0 ||
      selected->null_encoding != 1 ||
      selected->charset_uuid != profile.charset_uuid ||
      selected->collation_uuid != profile.collation_uuid ||
      selected->descriptor_kind != "canonical_type_descriptor" ||
      selected->canonical_type_name != "text" ||
      selected->encoded_type_descriptor.empty() ||
      !character_limit_matches) {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "CTB.TEXT.DESCRIPTOR_INVALID",
          "engine.contextual_text_literal.target_projection_column_mismatch");
    }
    return false;
  }
  EngineDescriptor resolved;
  resolved.descriptor_uuid.canonical = UuidText(selected->descriptor_uuid);
  // Public MGA projection rows retain the persisted descriptor class.  The
  // executor-facing value descriptor is the scalar view of that exact column;
  // its UUID, type and encoded authority remain byte-for-byte unchanged.
  resolved.descriptor_kind = "scalar";
  resolved.canonical_type_name = selected->canonical_type_name;
  resolved.encoded_descriptor = selected->encoded_type_descriptor;
  *descriptor = std::move(resolved);
  if (diagnostic != nullptr) *diagnostic = OkDiagnostic();
  return true;
}

std::atomic<std::uint64_t> g_identity_ordinal{1};
std::mutex g_authority_registry_mutex;
std::map<std::string,
         std::weak_ptr<EngineContextualTextLiteralAuthorityHandleV2::Authority>>
    g_authorities_by_receipt;

bool GenerateUuidV7(sblr::ContextualTextUuidV2* output) {
  if (output == nullptr) return false;
  const auto milliseconds = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  for (unsigned attempt = 0; attempt != 8; ++attempt) {
    const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
        scratchbird::core::platform::UuidKind::object,
        milliseconds + g_identity_ordinal.fetch_add(1,
                                                     std::memory_order_relaxed));
    if (!generated.ok()) continue;
    std::copy(generated.value.value.bytes.begin(),
              generated.value.value.bytes.end(), output->begin());
    if (Nonzero(*output)) return true;
  }
  *output = {};
  return false;
}

sblr::ContextualTextSha256V2 HashBytes(
    const std::vector<std::uint8_t>& bytes) {
  return scratchbird::core::hash::ComputeSha256Digest(bytes).digest;
}

void AppendU32(std::vector<std::uint8_t>* bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    bytes->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void AppendU64(std::vector<std::uint8_t>* bytes, std::uint64_t value) {
  for (unsigned shift = 0; shift != 64; shift += 8) {
    bytes->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void AppendUuid(std::vector<std::uint8_t>* bytes,
                const sblr::ContextualTextUuidV2& value) {
  bytes->insert(bytes->end(), value.begin(), value.end());
}

void AppendSha(std::vector<std::uint8_t>* bytes,
               const sblr::ContextualTextSha256V2& value) {
  bytes->insert(bytes->end(), value.begin(), value.end());
}

bool DecodeStoredSha256(std::string_view text,
                        sblr::ContextualTextSha256V2* out) {
  if (out == nullptr || text.size() != 71 ||
      !text.starts_with("sha256:")) {
    return false;
  }
  const auto nibble = [](char value) -> int {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
  };
  for (std::size_t index = 0; index != out->size(); ++index) {
    const auto high = nibble(text[7 + 2 * index]);
    const auto low = nibble(text[8 + 2 * index]);
    if (high < 0 || low < 0) {
      *out = {};
      return false;
    }
    (*out)[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

std::uint16_t ReadU16(const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>(bytes[0]) |
         (static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t ReadU32(const std::uint8_t* bytes) {
  std::uint32_t value = 0;
  for (unsigned index = 0; index != 4; ++index) {
    value |= static_cast<std::uint32_t>(bytes[index]) << (8 * index);
  }
  return value;
}

std::uint64_t ReadU64(const std::uint8_t* bytes) {
  std::uint64_t value = 0;
  for (unsigned index = 0; index != 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (8 * index);
  }
  return value;
}

bool ExactSbelMatchesTransfer(
    const std::vector<std::uint8_t>& exact_sbel,
    const EngineContextualTextComposedTransferRecordV2& transfer,
    const sblr::ContextualTextSha256V2* exact_sbos_sha256) {
  return exact_sbel.size() == 176 &&
         std::equal(exact_sbel.begin(), exact_sbel.begin() + 4,
                    reinterpret_cast<const std::uint8_t*>("SBEL")) &&
         ReadU16(exact_sbel.data() + 4) == 1 &&
         ReadU16(exact_sbel.data() + 6) == 176 &&
         ReadU32(exact_sbel.data() + 8) == 176 &&
         ReadU32(exact_sbel.data() + 12) == 0 &&
         std::equal(exact_sbel.begin() + 16, exact_sbel.begin() + 32,
                    transfer.final_receipt_uuid.begin()) &&
         std::equal(exact_sbel.begin() + 32, exact_sbel.begin() + 48,
                    transfer.admission_token_uuid.begin()) &&
         std::equal(exact_sbel.begin() + 48, exact_sbel.begin() + 80,
                    transfer.admission_token_binding_sha256.begin()) &&
         std::equal(exact_sbel.begin() + 80, exact_sbel.begin() + 112,
                    transfer.bound_ast_sha256.begin()) &&
         std::equal(exact_sbel.begin() + 112, exact_sbel.begin() + 144,
                    transfer.complete_sbxn_sha256.begin()) &&
         (exact_sbos_sha256 == nullptr ||
          std::equal(exact_sbel.begin() + 144, exact_sbel.end(),
                     exact_sbos_sha256->begin()));
}

bool SamePinnedContext(const EngineRequestContext& left,
                       const EngineRequestContext& right) {
  return left.database_uuid.canonical == right.database_uuid.canonical &&
         left.session_uuid.canonical == right.session_uuid.canonical &&
         left.transaction_uuid.canonical == right.transaction_uuid.canonical &&
         left.statement_uuid.canonical == right.statement_uuid.canonical &&
         left.statement_receipt_uuid.canonical ==
             right.statement_receipt_uuid.canonical &&
         left.statement_snapshot_uuid.canonical ==
             right.statement_snapshot_uuid.canonical &&
         left.datatype_catalog_snapshot_uuid.canonical ==
             right.datatype_catalog_snapshot_uuid.canonical &&
         left.datatype_catalog_generation == right.datatype_catalog_generation &&
         left.datatype_registry_generation ==
             right.datatype_registry_generation &&
         left.security_epoch == right.security_epoch &&
         left.resource_epoch == right.resource_epoch;
}

bool RequestMatchesContext(
    const EngineRequestContext& context,
    const sblr::ContextualTextLiteralNegotiationRequestV2& request) {
  sblr::ContextualTextUuidV2 statement_receipt{};
  sblr::ContextualTextUuidV2 catalog_snapshot{};
  sblr::ContextualTextUuidV2 mga_snapshot{};
  return context.security_context_present &&
         ToWireUuid(context.statement_receipt_uuid.canonical,
                    &statement_receipt) &&
         ToWireUuid(context.datatype_catalog_snapshot_uuid.canonical,
                    &catalog_snapshot) &&
         ToWireUuid(context.statement_snapshot_uuid.canonical, &mga_snapshot) &&
         statement_receipt == request.statement_receipt_uuid &&
         catalog_snapshot == request.catalog_snapshot_uuid &&
         mga_snapshot == request.mga_snapshot_uuid &&
         context.datatype_catalog_generation == request.catalog_generation &&
         context.datatype_registry_generation ==
             request.datatype_registry_generation &&
         context.security_epoch == request.security_generation &&
         context.resource_epoch == request.resource_epoch;
}

SblrExecutorAvailabilityRowIdentity ContextualExecutorIdentity() {
  SblrExecutorAvailabilityRowIdentity identity;
  identity.executor_id = kSblrContextualTextLiteralExecutorId;
  identity.opcode_code = kSblrContextualTextLiteralOpcodeCode;
  identity.opcode_version = kSblrContextualTextLiteralOpcodeVersion;
  identity.operand_descriptor_id =
      kSblrContextualTextLiteralOperandDescriptorId;
  identity.result_descriptor_id =
      kSblrContextualTextLiteralResultDescriptorId;
  identity.result_descriptor_version =
      kSblrContextualTextLiteralResultDescriptorVersion;
  return identity;
}

bool ValidExecutorAvailability(
    const SblrExecutorAvailabilitySnapshot& availability,
    const EngineRequestContext& context) {
  return ExactUuid(availability.snapshot_uuid) && availability.generation != 0 &&
         availability.database_uuid == context.database_uuid.canonical &&
         !availability.row_identity_sha256.empty() && availability.installed &&
         availability.availability_state ==
             SblrExecutorAvailabilityState::installed &&
         !availability.decision_evidence_sha256.empty() &&
         availability.row_identity_sha256 ==
             ComputeSblrExecutorAvailabilityRowIdentitySha256(
                 ContextualExecutorIdentity());
}

EngineApiDiagnostic CodecDiagnostic(
    const sblr::ContextualTextCodecDiagnosticV2& diagnostic) {
  return Diagnostic(
      diagnostic.code.empty() ? "SBLR.CONTEXTUAL_TEXT_LITERAL.NON_CANONICAL"
                              : diagnostic.code,
      "engine.contextual_text_literal.codec_refused", diagnostic.detail);
}

enum class LogicalPayloadAccountingV2 : std::uint8_t {
  retained_capacity,
  copied_size,
};

constexpr bool CheckedAddLogicalBytesV2(const std::uint64_t left,
                                        const std::uint64_t right,
                                        std::uint64_t* out) noexcept {
  if (out == nullptr ||
      right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  *out = left + right;
  return true;
}

constexpr bool CheckedMultiplyLogicalBytesV2(const std::uint64_t count,
                                             const std::uint64_t width,
                                             std::uint64_t* out) noexcept {
  if (out == nullptr ||
      (width != 0 &&
       count > std::numeric_limits<std::uint64_t>::max() / width)) {
    return false;
  }
  *out = count * width;
  return true;
}

static_assert([] {
  std::uint64_t result = 0;
  return !CheckedAddLogicalBytesV2(
             std::numeric_limits<std::uint64_t>::max(), 1, &result) &&
         !CheckedMultiplyLogicalBytesV2(
             std::numeric_limits<std::uint64_t>::max(), 2, &result) &&
         CheckedAddLogicalBytesV2(1, 2, &result) && result == 3;
}());

class LogicalByteCounterV2 final {
 public:
  void Add(const std::uint64_t bytes) noexcept {
    if (!ok_) return;
    ok_ = CheckedAddLogicalBytesV2(value_, bytes, &value_);
  }

  void AddSize(const std::size_t bytes) noexcept {
    static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
    Add(static_cast<std::uint64_t>(bytes));
  }

  void AddProduct(const std::size_t count,
                  const std::size_t width) noexcept {
    static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
    std::uint64_t product = 0;
    if (!ok_ ||
        !CheckedMultiplyLogicalBytesV2(
            static_cast<std::uint64_t>(count),
            static_cast<std::uint64_t>(width), &product)) {
      ok_ = false;
      return;
    }
    Add(product);
  }

  template <typename T>
  void AddObject() noexcept {
    AddSize(sizeof(T));
  }

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] std::uint64_t value() const noexcept { return value_; }

 private:
  std::uint64_t value_ = 0;
  bool ok_ = true;
};

std::size_t LogicalPayloadUnitsV2(
    const std::string& value,
    const LogicalPayloadAccountingV2 accounting) noexcept {
  return accounting == LogicalPayloadAccountingV2::retained_capacity
             ? value.capacity()
             : value.size();
}

template <typename T>
std::size_t LogicalPayloadUnitsV2(
    const std::vector<T>& value,
    const LogicalPayloadAccountingV2 accounting) noexcept {
  return accounting == LogicalPayloadAccountingV2::retained_capacity
             ? value.capacity()
             : value.size();
}

void AddStringPayloadV2(const std::string& value,
                        const LogicalPayloadAccountingV2 accounting,
                        LogicalByteCounterV2* counter) noexcept {
  counter->AddProduct(LogicalPayloadUnitsV2(value, accounting), sizeof(char));
}

template <typename T>
void AddVectorStorageV2(const std::vector<T>& value,
                        const LogicalPayloadAccountingV2 accounting,
                        LogicalByteCounterV2* counter) noexcept {
  counter->AddProduct(LogicalPayloadUnitsV2(value, accounting), sizeof(T));
}

void AddDynamicPayloadV2(const EngineUuid& value,
                         const LogicalPayloadAccountingV2 accounting,
                         LogicalByteCounterV2* counter) noexcept {
  AddStringPayloadV2(value.canonical, accounting, counter);
}

void AddDynamicPayloadV2(const EngineDescriptor& value,
                         const LogicalPayloadAccountingV2 accounting,
                         LogicalByteCounterV2* counter) noexcept {
  AddDynamicPayloadV2(value.descriptor_uuid, accounting, counter);
  AddStringPayloadV2(value.descriptor_kind, accounting, counter);
  AddStringPayloadV2(value.canonical_type_name, accounting, counter);
  AddStringPayloadV2(value.encoded_descriptor, accounting, counter);
}

void AddDynamicPayloadV2(const EngineTypedValue& value,
                         const LogicalPayloadAccountingV2 accounting,
                         LogicalByteCounterV2* counter) noexcept {
  AddDynamicPayloadV2(value.descriptor, accounting, counter);
  AddStringPayloadV2(value.encoded_value, accounting, counter);
  AddVectorStorageV2(value.binary_value, accounting, counter);
}

void AddDynamicPayloadV2(
    const scratchbird::core::datatypes::DatatypeTextSeedAuthority& value,
    const LogicalPayloadAccountingV2 accounting,
    LogicalByteCounterV2* counter) noexcept {
  AddStringPayloadV2(value.seed_pack_name, accounting, counter);
  AddStringPayloadV2(value.seed_pack_version, accounting, counter);
  AddStringPayloadV2(value.charset_name, accounting, counter);
  AddStringPayloadV2(value.collation_name, accounting, counter);
}

void AddDynamicPayloadV2(const EngineResolvedResourceDescriptor& value,
                         const LogicalPayloadAccountingV2 accounting,
                         LogicalByteCounterV2* counter) noexcept {
  AddStringPayloadV2(value.resource_family, accounting, counter);
  AddStringPayloadV2(value.canonical_name, accounting, counter);
  AddDynamicPayloadV2(value.resource_uuid, accounting, counter);
  AddDynamicPayloadV2(value.parent_resource_uuid, accounting, counter);
  AddStringPayloadV2(value.parent_canonical_name, accounting, counter);
  AddDynamicPayloadV2(value.default_collation_uuid, accounting, counter);
  AddStringPayloadV2(value.default_collation_name, accounting, counter);
  AddStringPayloadV2(value.seed_pack_name, accounting, counter);
  AddStringPayloadV2(value.seed_pack_version, accounting, counter);
  AddStringPayloadV2(value.family_version, accounting, counter);
}

void AddDiagnosticLiteralPayloadV2(const std::string_view code,
                                   const std::string_view message_key,
                                   const std::string_view detail,
                                   LogicalByteCounterV2* counter) noexcept {
  counter->AddSize(code.size());
  counter->AddSize(message_key.size());
  counter->AddSize(detail.size());
}

void AddDynamicPayloadV2(
    const SblrExecutorAvailabilitySnapshot& value,
    const LogicalPayloadAccountingV2 accounting,
    LogicalByteCounterV2* counter) noexcept {
  AddStringPayloadV2(value.snapshot_uuid, accounting, counter);
  AddStringPayloadV2(value.database_uuid, accounting, counter);
  AddStringPayloadV2(value.row_identity_sha256, accounting, counter);
  AddStringPayloadV2(value.decision_evidence_sha256, accounting, counter);
}

void AddDynamicPayloadV2(
    const sblr::ContextualTextLiteralDemandV2& value,
    const LogicalPayloadAccountingV2 accounting,
    LogicalByteCounterV2* counter) noexcept {
  AddVectorStorageV2(value.raw_token, accounting, counter);
  AddVectorStorageV2(value.lexical_value, accounting, counter);
}

void AddDynamicPayloadV2(
    const sblr::ContextualTextLiteralNegotiationRequestV2& value,
    const LogicalPayloadAccountingV2 accounting,
    LogicalByteCounterV2* counter) noexcept {
  AddVectorStorageV2(value.demands, accounting, counter);
  for (const auto& demand : value.demands) {
    AddDynamicPayloadV2(demand, accounting, counter);
  }
}

void AddDynamicPayloadV2(const EngineResolvedContextualTextTargetV2& value,
                         const LogicalPayloadAccountingV2 accounting,
                         LogicalByteCounterV2* counter) noexcept {
  AddVectorStorageV2(value.exact_public_relation_projection_v3, accounting,
                     counter);
  AddVectorStorageV2(value.exact_sbtltd02, accounting, counter);
}

void AddDynamicPayloadV2(const PreparedContextualTextValueV2& value,
                         const LogicalPayloadAccountingV2 accounting,
                         LogicalByteCounterV2* counter) noexcept {
  AddStringPayloadV2(value.codec_id, accounting, counter);
  AddVectorStorageV2(value.canonical_body, accounting, counter);
}

void AddDynamicPayloadV2(
    const sblr::ContextualTextLiteralProfileV2& value,
    const LogicalPayloadAccountingV2 accounting,
    LogicalByteCounterV2* counter) noexcept {
  AddVectorStorageV2(value.canonical_body, accounting, counter);
  AddVectorStorageV2(value.exact_bytes, accounting, counter);
}

void AddDynamicPayloadV2(
    const EngineContextualTextVerifiedGraphBindingV2& value,
    const LogicalPayloadAccountingV2 accounting,
    LogicalByteCounterV2* counter) noexcept {
  for (const auto& field : value.exact_relational_descriptor_v2_fields) {
    AddStringPayloadV2(field, accounting, counter);
  }
  AddStringPayloadV2(value.canonical_type_name, accounting, counter);
  for (const auto& field :
       value.exact_target_relational_descriptor_v2_fields) {
    AddStringPayloadV2(field, accounting, counter);
  }
  AddStringPayloadV2(value.target_canonical_type_name, accounting, counter);
}

void AddDynamicPayloadV2(
    const EngineContextualTextComparisonResourceSnapshotV2& value,
    const LogicalPayloadAccountingV2 accounting,
    LogicalByteCounterV2* counter) noexcept {
  AddStringPayloadV2(value.charset_name, accounting, counter);
  AddStringPayloadV2(value.charset_uuid_canonical, accounting, counter);
  AddStringPayloadV2(value.collation_name, accounting, counter);
  AddStringPayloadV2(value.collation_uuid_canonical, accounting, counter);
  AddDynamicPayloadV2(value.text_seed, accounting, counter);
  AddDynamicPayloadV2(value.charset_resource, accounting, counter);
  AddDynamicPayloadV2(value.collation_resource, accounting, counter);
  AddVectorStorageV2(value.exact_public_relation_projection_v3, accounting,
                     counter);
  AddVectorStorageV2(value.exact_sbtltd02, accounting, counter);
}

void AddDynamicPayloadV2(
    const EngineContextualTextPreparedRuntimeMaterializationV2& value,
    const LogicalPayloadAccountingV2 accounting,
    LogicalByteCounterV2* counter) noexcept {
  AddDynamicPayloadV2(value.graph_binding, accounting, counter);
  AddDynamicPayloadV2(value.value, accounting, counter);
  AddDynamicPayloadV2(value.target_descriptor, accounting, counter);
  AddVectorStorageV2(value.exact_literal_relational_descriptor_v2_bytes,
                     accounting, counter);
  AddVectorStorageV2(value.exact_target_relational_descriptor_v2_bytes,
                     accounting, counter);
  AddDynamicPayloadV2(value.exact_profile, accounting, counter);
  AddDynamicPayloadV2(value.comparison_resources, accounting, counter);
}

void AddDynamicPayloadV2(
    const EngineContextualTextPreparedExecutionEntryV2& value,
    const LogicalPayloadAccountingV2 accounting,
    LogicalByteCounterV2* counter) noexcept {
  AddDynamicPayloadV2(value.prepared_value, accounting, counter);
  AddDynamicPayloadV2(value.runtime_materialization, accounting, counter);
}

void AddDynamicPayloadV2(
    const ContextualTextExecutionAuthorityEntryV2& value,
    const LogicalPayloadAccountingV2 accounting,
    LogicalByteCounterV2* counter) noexcept {
  AddDynamicPayloadV2(
      static_cast<const PreparedContextualTextValueV2&>(value.typed_value),
      accounting, counter);
  AddDynamicPayloadV2(value.runtime_materialization, accounting, counter);
}

void AddDynamicPayloadV2(
    const EngineContextualTextComposedTransferRecordV2& value,
    const LogicalPayloadAccountingV2 accounting,
    LogicalByteCounterV2* counter) noexcept {
  AddVectorStorageV2(value.exact_evidence_material, accounting, counter);
}

std::uint64_t FinalRevalidationIterationLogicalBytesV2(
    const EngineContextualTextPreparedExecutionEntryV2& entry,
    const EngineResolvedContextualTextTargetV2& target,
    bool* ok) noexcept {
  if (ok == nullptr) return 0;
  LogicalByteCounterV2 counter;
  // Caller current_resources, ResolveComparisonResources retained, and the
  // live-fallback resolved snapshot coexist as static objects. Only one owns
  // the full dynamic resource payload at a time after noexcept moves.
  counter.AddProduct(3,
                     sizeof(EngineContextualTextComparisonResourceSnapshotV2));
  AddDynamicPayloadV2(entry.runtime_materialization.comparison_resources,
                      LogicalPayloadAccountingV2::copied_size, &counter);

  // The live fallback decodes and canonically re-encodes SBTLTD02. Count the
  // decoded exact bytes, re-encoding, and the nested evidence-hash canonical
  // copy/material as a conservative provider-visible logical peak.
  counter.AddObject<sblr::ContextualTextDescriptorV2>();
  counter.AddObject<sblr::ContextualTextCodecDiagnosticV2>();
  counter.AddProduct(4, target.exact_sbtltd02.size());
  counter.AddSize(std::string_view(
                      "ScratchBird.ContextualText.TextDescriptor.V2")
                      .size() +
                  1);

  // The target-projection validation hash owns one canonical material vector.
  counter.AddSize(std::string_view(
                      "ScratchBird.ContextualText.TargetProjection.V2")
                      .size() +
                  1 + 8 + 4 + 4 + 16 + 8 + 4 + 4 + 4);
  counter.AddSize(target.exact_public_relation_projection_v3.size());

  // Live resource lookup returns two exact descriptors while constructing one
  // additional descriptor locally. Database/MGA loader scratch remains owned
  // by that external registry and is explicitly not part of this estimate.
  counter.AddProduct(2, sizeof(EngineResourceDescriptorLookupResult));
  counter.AddObject<EngineResolvedResourceDescriptor>();
  AddDynamicPayloadV2(
      entry.runtime_materialization.comparison_resources.charset_resource,
      LogicalPayloadAccountingV2::copied_size, &counter);
  AddDynamicPayloadV2(
      entry.runtime_materialization.comparison_resources.collation_resource,
      LogicalPayloadAccountingV2::copied_size, &counter);
  const auto& charset_resource =
      entry.runtime_materialization.comparison_resources.charset_resource;
  const auto& collation_resource =
      entry.runtime_materialization.comparison_resources.collation_resource;
  const auto* larger_resource = &charset_resource;
  LogicalByteCounterV2 charset_bytes;
  LogicalByteCounterV2 collation_bytes;
  AddDynamicPayloadV2(charset_resource,
                      LogicalPayloadAccountingV2::copied_size,
                      &charset_bytes);
  AddDynamicPayloadV2(collation_resource,
                      LogicalPayloadAccountingV2::copied_size,
                      &collation_bytes);
  if (collation_bytes.value() > charset_bytes.value()) {
    larger_resource = &collation_resource;
  }
  AddDynamicPayloadV2(*larger_resource,
                      LogicalPayloadAccountingV2::copied_size, &counter);
  counter.AddProduct(2, sizeof(EngineUuid));
  counter.AddProduct(2, 36);
  AddDiagnosticLiteralPayloadV2("SB_ENGINE_API_OK", "engine.api.ok", {},
                                &counter);
  AddDiagnosticLiteralPayloadV2("SB_ENGINE_API_OK", "engine.api.ok", {},
                                &counter);
  if (!charset_bytes.ok() || !collation_bytes.ok()) {
    *ok = false;
    return 0;
  }
  *ok = counter.ok();
  return counter.value();
}

}  // namespace

struct EngineContextualTextLiteralAuthorityHandleV2::Authority {
  mutable std::mutex mutex;
  EngineContextualTextLiteralAuthorityStateV2 state =
      EngineContextualTextLiteralAuthorityStateV2::issued;
  EngineRequestContext pinned_context;
  sblr::ContextualTextLiteralNegotiationRequestV2 request;
  sblr::ContextualTextLiteralProfileSetV2 profile_set;
  std::vector<std::uint8_t> exact_sbtlnr02;
  std::vector<std::uint8_t> exact_sbtlns02;
  std::vector<EngineResolvedContextualTextTargetV2> targets;
  SblrExecutorAvailabilitySnapshot executor_availability;
  std::optional<EngineContextualTextComposedTransferRecordV2>
      composed_transfer;
  const EngineContextualTextTargetAuthorityResolverV2* target_resolver =
      nullptr;
};

struct PreparedContextualTextLiteralSetV2::State {
  std::shared_ptr<EngineContextualTextLiteralAuthorityHandleV2::Authority>
      authority;
  std::vector<EngineContextualTextPreparedExecutionEntryV2> entries;
  std::vector<std::uint8_t> exact_sbel_v1;
  EngineContextualTextComposedTransferRecordV2 composed_transfer;
  sblr::ContextualTextSha256V2 sbos_sha256{};
  sblr::ContextualTextSha256V2 pre_contextual_operand_vector_sha256{};
  sblr::ContextualTextSha256V2 sbxn_sha256{};
  sblr::ContextualTextSha256V2 sbtlxe_sha256{};
};

struct ContextualTextExecutionAuthorityLeaseV2::State {
  std::shared_ptr<EngineContextualTextLiteralAuthorityHandleV2::Authority>
      authority;
  std::vector<ContextualTextExecutionAuthorityEntryV2> entries;
};

static_assert(std::is_nothrow_move_assignable_v<
              PreparedContextualTextValueV2>);
static_assert(std::is_nothrow_move_constructible_v<EngineTypedValue>);
static_assert(std::is_nothrow_move_assignable_v<EngineTypedValue>);
static_assert(std::is_nothrow_move_constructible_v<
              EngineContextualTextPreparedRuntimeMaterializationV2>);
static_assert(std::is_nothrow_move_assignable_v<
              EngineContextualTextPreparedRuntimeMaterializationV2>);
static_assert(std::is_nothrow_move_constructible_v<
              EngineContextualTextPreparedExecutionEntryV2>);
static_assert(std::is_nothrow_move_assignable_v<
              EngineContextualTextPreparedExecutionEntryV2>);
static_assert(!std::is_copy_constructible_v<
              EngineContextualTextPreparedExecutionEntryV2>);
static_assert(!std::is_copy_assignable_v<
              EngineContextualTextPreparedExecutionEntryV2>);
static_assert(std::is_nothrow_move_constructible_v<
              ContextualTextExecutionAuthorityEntryV2>);
static_assert(std::is_nothrow_move_assignable_v<
              ContextualTextExecutionAuthorityEntryV2>);
static_assert(std::is_nothrow_move_assignable_v<EngineApiDiagnostic>);
static_assert(std::is_nothrow_move_constructible_v<
              EngineContextualTextComposedTransferRecordV2>);
static_assert(std::is_nothrow_move_assignable_v<
              EngineContextualTextComposedTransferRecordV2>);

PreparedContextualTextLiteralSetV2::PreparedContextualTextLiteralSetV2() =
    default;
PreparedContextualTextLiteralSetV2::~PreparedContextualTextLiteralSetV2() =
    default;
PreparedContextualTextLiteralSetV2::PreparedContextualTextLiteralSetV2(
    PreparedContextualTextLiteralSetV2&&) noexcept = default;
PreparedContextualTextLiteralSetV2&
PreparedContextualTextLiteralSetV2::operator=(
    PreparedContextualTextLiteralSetV2&&) noexcept = default;

ContextualTextExecutionAuthorityLeaseV2::
    ContextualTextExecutionAuthorityLeaseV2() = default;
ContextualTextExecutionAuthorityLeaseV2::
    ~ContextualTextExecutionAuthorityLeaseV2() = default;
ContextualTextExecutionAuthorityLeaseV2::
    ContextualTextExecutionAuthorityLeaseV2(
        ContextualTextExecutionAuthorityLeaseV2&&) noexcept = default;
ContextualTextExecutionAuthorityLeaseV2&
ContextualTextExecutionAuthorityLeaseV2::operator=(
    ContextualTextExecutionAuthorityLeaseV2&&) noexcept = default;

sblr::SblrExpressionNodeTableCodecResultV1
DecodeContextualTextComposedSbxnV2(const std::uint8_t* bytes,
                                   const std::size_t size) {
  return sblr::DecodeSblrContextualComposedExpressionNodeTableV2(bytes,
                                                                 size);
}

bool DecodeContextualTextComposedLiteralFinalizeRequestV2(
    const std::uint8_t* bytes, const std::size_t size,
    sblr::SblrLiteralFinalizeRequestV1* out) {
  constexpr std::size_t kHeaderBytes = 208;
  constexpr std::size_t kMaximumBoundAstBytes = 491592;
  if (bytes == nullptr || out == nullptr || size < kHeaderBytes ||
      size > kContextualTextComposedMaximumSblfBytesV2 ||
      !std::equal(bytes, bytes + 4,
                  reinterpret_cast<const std::uint8_t*>("SBLF")) ||
      ReadU16(bytes + 4) != 1 || ReadU16(bytes + 6) != kHeaderBytes ||
      ReadU32(bytes + 8) != size || ReadU32(bytes + 12) != 0) {
    return false;
  }
  const auto sbba_bytes = ReadU32(bytes + 200);
  const auto sbxn_bytes = ReadU32(bytes + 204);
  if (sbba_bytes > kMaximumBoundAstBytes ||
      sbxn_bytes > kContextualTextComposedMaximumSbxnBytesV2 ||
      sbba_bytes > size - kHeaderBytes ||
      sbxn_bytes > size - kHeaderBytes - sbba_bytes ||
      kHeaderBytes + static_cast<std::size_t>(sbba_bytes) +
              static_cast<std::size_t>(sbxn_bytes) !=
          size) {
    return false;
  }
  sblr::SblrLiteralFinalizeRequestV1 decoded;
  std::copy_n(bytes + 16, decoded.preliminary_receipt_uuid.size(),
              decoded.preliminary_receipt_uuid.begin());
  std::copy_n(bytes + 32, decoded.demand_sha256.size(),
              decoded.demand_sha256.begin());
  std::copy_n(bytes + 64, decoded.ordered_profile_sha256.size(),
              decoded.ordered_profile_sha256.begin());
  std::copy_n(bytes + 96, decoded.bound_ast_sha256.size(),
              decoded.bound_ast_sha256.begin());
  std::copy_n(bytes + 128, decoded.sbxn_sha256.size(),
              decoded.sbxn_sha256.begin());
  decoded.catalog_generation = ReadU64(bytes + 160);
  decoded.security_epoch = ReadU64(bytes + 168);
  decoded.resource_epoch = ReadU64(bytes + 176);
  std::copy_n(bytes + 184, decoded.mga_snapshot_uuid.size(),
              decoded.mga_snapshot_uuid.begin());
  if (!Nonzero(decoded.preliminary_receipt_uuid) ||
      decoded.catalog_generation == 0 || !Nonzero(decoded.mga_snapshot_uuid) ||
      sbxn_bytes == 0) {
    return false;
  }
  decoded.canonical_sbba.assign(bytes + kHeaderBytes,
                                bytes + kHeaderBytes + sbba_bytes);
  decoded.canonical_sbxn.assign(bytes + kHeaderBytes + sbba_bytes,
                                bytes + size);
  sblr::SblrLiteralBoundAstV1 bound;
  if (!sblr::DecodeSblrLiteralBoundAstV1(decoded.canonical_sbba.data(),
                                         decoded.canonical_sbba.size(),
                                         &bound) ||
      sblr::ComputeSblrLiteralBoundAstSha256V1(decoded.canonical_sbba) !=
          decoded.bound_ast_sha256) {
    return false;
  }
  const auto table = DecodeContextualTextComposedSbxnV2(
      decoded.canonical_sbxn.data(), decoded.canonical_sbxn.size());
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      decoded.canonical_sbxn);
  if (!table.ok || !digest.ok() || digest.digest != decoded.sbxn_sha256) {
    return false;
  }
  *out = std::move(decoded);
  return true;
}

EngineContextualTextLiteralAuthorityIssueResultV2
IssueContextualTextLiteralAuthorityV2(
    const EngineContextualTextLiteralAuthorityIssueRequestV2& request) {
  EngineContextualTextLiteralAuthorityIssueResultV2 result;
  if (request.target_resolver == nullptr) {
    result.diagnostic = Diagnostic(
        "CTB.TEXT.DESCRIPTOR_INVALID",
        "engine.contextual_text_literal.engine_authority_missing",
        "a sealed target and contextual-policy resolver is required");
    return result;
  }
  if (request.context.query_cancellation_requested &&
      request.context.query_cancellation_requested()) {
    result.diagnostic = Diagnostic(
        "PROCESS.CANCELLED", "engine.contextual_text_literal.cancelled");
    return result;
  }

  sblr::ContextualTextLiteralNegotiationRequestV2 decoded_request;
  sblr::ContextualTextCodecDiagnosticV2 codec_diagnostic;
  if (!sblr::DecodeContextualTextLiteralNegotiationRequestV2(
          request.exact_sbtlnr02.data(), request.exact_sbtlnr02.size(),
          &decoded_request, &codec_diagnostic)) {
    result.diagnostic = CodecDiagnostic(codec_diagnostic);
    return result;
  }
  if (!RequestMatchesContext(request.context, decoded_request)) {
    result.diagnostic = Diagnostic(
        "ENGINE.STATEMENT_CONTEXT.RECEIPT_MISMATCH",
        "engine.contextual_text_literal.receipt_mismatch");
    return result;
  }

  EngineContextualTextLiteralBudgetV2 budget;
  EngineApiDiagnostic resolver_diagnostic;
  if (!request.target_resolver->BindBudget(
          request.context, decoded_request, request.exact_sbtlnr02.size(),
          &budget, &resolver_diagnostic)) {
    result.diagnostic = std::move(resolver_diagnostic);
    return result;
  }
  if (budget.literal_negotiation_byte_grant < 4096 ||
      budget.literal_negotiation_byte_grant >
          sblr::kContextualTextMaximumLogicalCarrierBytesV2 ||
      budget.canonical_body_aggregate_grant == 0 ||
      budget.canonical_body_aggregate_grant >
          sblr::kContextualTextMaximumBodyBytesV2 ||
      budget.canonical_body_aggregate_grant >
          budget.literal_negotiation_byte_grant ||
      request.exact_sbtlnr02.size() >
          budget.literal_negotiation_byte_grant) {
    result.diagnostic = Diagnostic(
        "SBLR.CONTEXTUAL_TEXT_LITERAL.BUDGET_EXCEEDED",
        "engine.contextual_text_literal.budget_invalid");
    return result;
  }

  const auto availability_load = LoadSblrExecutorAvailabilitySnapshot(
      request.context, ContextualExecutorIdentity());
  if (!availability_load.ok) {
    result.diagnostic = availability_load.diagnostic;
    return result;
  }
  const auto executor_availability = availability_load.snapshot;
  if (!ValidExecutorAvailability(executor_availability, request.context)) {
    result.diagnostic = Diagnostic(
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
        "engine.contextual_text_literal.executor_evidence_invalid");
    return result;
  }

  sblr::ContextualTextUuidV2 profile_set_uuid{};
  sblr::ContextualTextUuidV2 budget_uuid{};
  if (!GenerateUuidV7(&profile_set_uuid) || !GenerateUuidV7(&budget_uuid)) {
    result.diagnostic = Diagnostic(
        "ENGINE.INTERNAL.ERROR",
        "engine.contextual_text_literal.identity_generation_failed");
    return result;
  }
  std::set<std::string> issued_identities{Hex(profile_set_uuid),
                                          Hex(budget_uuid)};
  std::map<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>,
           sblr::ContextualTextUuidV2>
      source_occurrences;
  std::vector<EngineResolvedContextualTextTargetV2> targets;
  sblr::ContextualTextLiteralProfileSetV2 profile_set;
  profile_set.statement_receipt_uuid = decoded_request.statement_receipt_uuid;
  profile_set.profile_set_uuid = profile_set_uuid;
  profile_set.profile_set_generation = 1;
  profile_set.catalog_snapshot_uuid = decoded_request.catalog_snapshot_uuid;
  profile_set.catalog_generation = decoded_request.catalog_generation;
  profile_set.datatype_registry_generation =
      decoded_request.datatype_registry_generation;
  profile_set.security_generation = decoded_request.security_generation;
  profile_set.resource_epoch = decoded_request.resource_epoch;
  profile_set.mga_snapshot_uuid = decoded_request.mga_snapshot_uuid;
  profile_set.literal_budget_uuid = budget_uuid;
  profile_set.literal_budget_generation = 1;
  profile_set.literal_negotiation_byte_grant =
      budget.literal_negotiation_byte_grant;
  profile_set.canonical_body_aggregate_grant =
      budget.canonical_body_aggregate_grant;
  profile_set.demand_sequence_sha256 = decoded_request.demand_sequence_sha256;

  std::uint64_t canonical_body_sum = 0;
  for (const auto& demand : decoded_request.demands) {
    EngineResolvedContextualTextTargetV2 target;
    if (!request.target_resolver->ResolveTarget(
            request.context, demand, &target, &resolver_diagnostic)) {
      result.diagnostic = std::move(resolver_diagnostic);
      return result;
    }
    if (target.literal_occurrence != demand.literal_occurrence ||
        target.exact_public_relation_projection_v3.empty() ||
        target.exact_sbtltd02.empty()) {
      result.diagnostic = Diagnostic(
          "SBLR.CONTEXTUAL_TEXT_LITERAL.TARGET_MISMATCH",
          "engine.contextual_text_literal.target_resolution_invalid");
      return result;
    }
    sblr::ContextualTextDescriptorV2 descriptor;
    if (!sblr::DecodeContextualTextDescriptorV2(
            target.exact_sbtltd02.data(), target.exact_sbtltd02.size(),
            &descriptor, &codec_diagnostic)) {
      result.diagnostic = CodecDiagnostic(codec_diagnostic);
      return result;
    }
    if (descriptor.resource_epoch != decoded_request.resource_epoch) {
      result.diagnostic = Diagnostic(
          "CTB.TEXT.RESOURCE_EPOCH_MISMATCH",
          "engine.contextual_text_literal.target_resource_epoch_mismatch");
      return result;
    }
    if (demand.scalar_count > descriptor.character_limit &&
        descriptor.character_limit != std::numeric_limits<std::uint64_t>::max()) {
      result.diagnostic = Diagnostic(
          "CTB.TEXT.LENGTH_EXCEEDED",
          "engine.contextual_text_literal.target_character_limit_exceeded");
      return result;
    }
    if (demand.lexical_value.size() > descriptor.byte_limit &&
        descriptor.byte_limit != std::numeric_limits<std::uint64_t>::max()) {
      result.diagnostic = Diagnostic(
          "CTB.TEXT.LENGTH_EXCEEDED",
          "engine.contextual_text_literal.target_byte_limit_exceeded");
      return result;
    }
    if (demand.lexical_value.size() >
        std::numeric_limits<std::uint64_t>::max() - canonical_body_sum) {
      result.diagnostic = Diagnostic(
          "SBLR.CONTEXTUAL_TEXT_LITERAL.BUDGET_EXCEEDED",
          "engine.contextual_text_literal.body_aggregate_overflow");
      return result;
    }
    canonical_body_sum += demand.lexical_value.size();
    if (canonical_body_sum > budget.canonical_body_aggregate_grant) {
      result.diagnostic = Diagnostic(
          "SBLR.CONTEXTUAL_TEXT_LITERAL.BUDGET_EXCEEDED",
          "engine.contextual_text_literal.body_aggregate_exceeded");
      return result;
    }

    const auto source_key = std::make_tuple(
        demand.source_node_id, demand.source_operand_ordinal,
        demand.source_ordinal);
    auto source = source_occurrences.find(source_key);
    if (source == source_occurrences.end()) {
      sblr::ContextualTextUuidV2 issued{};
      do {
        if (!GenerateUuidV7(&issued)) {
          result.diagnostic = Diagnostic(
              "ENGINE.INTERNAL.ERROR",
              "engine.contextual_text_literal.identity_generation_failed");
          return result;
        }
      } while (!issued_identities.insert(Hex(issued)).second);
      source = source_occurrences.emplace(source_key, issued).first;
    }

    sblr::ContextualTextLiteralProfileV2 profile;
    auto generate_distinct = [&](sblr::ContextualTextUuidV2* output) {
      do {
        if (!GenerateUuidV7(output)) return false;
      } while (!issued_identities.insert(Hex(*output)).second);
      return true;
    };
    if (!generate_distinct(&profile.profile_uuid) ||
        !generate_distinct(&profile.literal_binding_uuid)) {
      result.diagnostic = Diagnostic(
          "ENGINE.INTERNAL.ERROR",
          "engine.contextual_text_literal.identity_generation_failed");
      return result;
    }
    profile.profile_set_uuid = profile_set.profile_set_uuid;
    profile.profile_set_generation = 1;
    profile.literal_binding_generation = 1;
    profile.literal_occurrence = demand.literal_occurrence;
    profile.node_id = demand.node_id;
    profile.comparison_occurrence = demand.comparison_occurrence;
    profile.statement_receipt_uuid = decoded_request.statement_receipt_uuid;
    profile.catalog_snapshot_uuid = decoded_request.catalog_snapshot_uuid;
    profile.catalog_generation = decoded_request.catalog_generation;
    profile.datatype_registry_generation =
        decoded_request.datatype_registry_generation;
    profile.security_generation = decoded_request.security_generation;
    profile.resource_epoch = decoded_request.resource_epoch;
    profile.mga_snapshot_uuid = decoded_request.mga_snapshot_uuid;
    profile.descriptor_uuid = descriptor.descriptor_uuid;
    profile.descriptor_generation = descriptor.descriptor_generation;
    profile.type_uuid = descriptor.type_uuid;
    profile.type_generation = descriptor.type_generation;
    profile.codec_uuid = descriptor.codec_uuid;
    profile.codec_version = descriptor.codec_version;
    profile.codec_generation = descriptor.codec_generation;
    profile.literal_argument_ordinal = demand.literal_argument_ordinal;
    profile.target_argument_ordinal = demand.target_argument_ordinal;
    profile.source_occurrence_uuid = source->second;
    profile.source_generation = 1;
    profile.relation_uuid = demand.relation_uuid;
    profile.relation_descriptor_uuid = demand.relation_descriptor_uuid;
    profile.relation_descriptor_generation =
        demand.relation_descriptor_generation;
    profile.column_uuid = demand.column_uuid;
    profile.column_ordinal = demand.column_ordinal;
    profile.parent_operand_ordinal = demand.parent_operand_ordinal;
    profile.target_descriptor_handle = demand.target_descriptor_handle;
    profile.literal_descriptor_handle = demand.literal_descriptor_handle;
    profile.scalar_count = demand.scalar_count;
    profile.target_character_limit = descriptor.character_limit;
    profile.target_byte_limit = descriptor.byte_limit;
    profile.charset_uuid = descriptor.charset_uuid;
    profile.charset_generation = descriptor.charset_generation;
    profile.collation_uuid = descriptor.collation_uuid;
    profile.collation_generation = descriptor.collation_generation;
    profile.normalization_policy_uuid = descriptor.normalization_policy_uuid;
    profile.normalization_policy_generation =
        descriptor.normalization_policy_generation;
    profile.padding_policy_uuid = descriptor.padding_policy_uuid;
    profile.padding_policy_generation = descriptor.padding_policy_generation;
    profile.case_accent_policy_uuid = descriptor.case_accent_policy_uuid;
    profile.case_accent_policy_generation =
        descriptor.case_accent_policy_generation;
    profile.render_policy_uuid = descriptor.render_policy_uuid;
    profile.render_policy_generation = descriptor.render_policy_generation;
    profile.canonicalization_profile_uuid =
        descriptor.canonicalization_profile_uuid;
    profile.canonicalization_profile_generation =
        descriptor.canonicalization_profile_generation;
    profile.comparison_contract_uuid = descriptor.comparison_contract_uuid;
    profile.comparison_contract_generation =
        descriptor.comparison_contract_generation;
    profile.equality_operation_uuid = descriptor.equality_operation_uuid;
    profile.equality_operation_generation =
        descriptor.equality_operation_generation;
    profile.literal_budget_uuid = profile_set.literal_budget_uuid;
    profile.literal_budget_generation = 1;
    profile.literal_negotiation_byte_grant =
        profile_set.literal_negotiation_byte_grant;
    profile.canonical_body_aggregate_grant =
        profile_set.canonical_body_aggregate_grant;
    profile.raw_token_sha256 = demand.raw_token_sha256;
    profile.lexical_value_sha256 = demand.lexical_value_sha256;
    profile.descriptor_evidence_sha256 =
        descriptor.descriptor_evidence_sha256;
    profile.target_projection_sha256 =
        sblr::ComputeContextualTextTargetProjectionSha256V2(
            demand, source->second, 1,
            target.exact_public_relation_projection_v3);
    profile.target_context_sha256 =
        sblr::ComputeContextualTextTargetContextSha256V2(
            decoded_request, demand, source->second, 1, descriptor,
            profile.target_projection_sha256,
            profile.descriptor_evidence_sha256);
    profile.demand_sequence_sha256 = decoded_request.demand_sequence_sha256;
    // Generation-1 canonicalization is identity after the independently
    // verified raw-token decode performed by the request codec.
    profile.canonical_body = demand.lexical_value;

    sblr::ContextualTextLiteralProfileMappingV2 mapping;
    mapping.literal_occurrence = demand.literal_occurrence;
    mapping.node_id = demand.node_id;
    mapping.literal_binding_uuid = profile.literal_binding_uuid;
    mapping.literal_binding_generation = 1;
    mapping.literal_descriptor_handle = demand.literal_descriptor_handle;
    mapping.target_descriptor_handle = demand.target_descriptor_handle;
    mapping.profile = std::move(profile);
    profile_set.mappings.push_back(std::move(mapping));
    targets.push_back(std::move(target));
  }

  std::vector<std::uint8_t> exact_result;
  if (!sblr::EncodeContextualTextLiteralProfileSetV2(
          profile_set, &exact_result, &codec_diagnostic)) {
    result.diagnostic = CodecDiagnostic(codec_diagnostic);
    return result;
  }
  sblr::ContextualTextLiteralProfileSetV2 canonical_profile_set;
  if (!sblr::DecodeContextualTextLiteralProfileSetV2(
          exact_result.data(), exact_result.size(), &canonical_profile_set,
          &codec_diagnostic)) {
    result.diagnostic = CodecDiagnostic(codec_diagnostic);
    return result;
  }
  sblr::ContextualTextLiteralExecuteV2 prospective_execute;
  static_cast<sblr::ContextualTextLiteralProfileSetV2&>(prospective_execute) =
      canonical_profile_set;
  prospective_execute.pre_contextual_operand_vector_sha256[0] = 1;
  prospective_execute.sbxn_sha256[0] = 1;
  std::vector<std::uint8_t> exact_prospective_execute;
  if (!sblr::EncodeContextualTextLiteralExecuteV2(
          prospective_execute, &exact_prospective_execute,
          &codec_diagnostic)) {
    result.diagnostic = CodecDiagnostic(codec_diagnostic);
    return result;
  }

  std::shared_ptr<EngineContextualTextLiteralAuthorityHandleV2::Authority>
      authority;
  try {
    authority = std::make_shared<
        EngineContextualTextLiteralAuthorityHandleV2::Authority>();
    authority->pinned_context = request.context;
    authority->request = decoded_request;
    authority->profile_set = canonical_profile_set;
    authority->exact_sbtlnr02 = request.exact_sbtlnr02;
    authority->exact_sbtlns02 = exact_result;
    authority->targets = std::move(targets);
    authority->executor_availability = executor_availability;
    authority->target_resolver = request.target_resolver;
  } catch (const std::bad_alloc&) {
    result.diagnostic = Diagnostic(
        "ENGINE.RESOURCE.EXHAUSTED",
        "engine.contextual_text_literal.authority_allocation_failed");
    return result;
  }

  const std::string receipt_key = Hex(decoded_request.statement_receipt_uuid);
  {
    std::lock_guard<std::mutex> guard(g_authority_registry_mutex);
    const auto existing = g_authorities_by_receipt.find(receipt_key);
    if (existing != g_authorities_by_receipt.end() &&
        !existing->second.expired()) {
      result.diagnostic = Diagnostic(
          "SBLR.CONTEXTUAL_TEXT_LITERAL.REPLAY",
          "engine.contextual_text_literal.second_set_refused");
      return result;
    }
    g_authorities_by_receipt[receipt_key] = authority;
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.profile_set = canonical_profile_set;
  result.exact_sbtlns02 = std::move(exact_result);
  result.authority.authority_ = std::move(authority);
  return result;
}

bool ValidateContextualTextComposedSbxnPartitionV2(
    const EngineRequestContext& context,
    const EngineContextualTextLiteralAuthorityHandleV2& authority_handle,
    const std::vector<std::uint8_t>& exact_sbxn,
    const std::vector<std::uint64_t>& numeric_node_ids,
    EngineApiDiagnostic* diagnostic) {
  if (!authority_handle.valid() || exact_sbxn.empty()) {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "SBLR.OPERAND_INVALID",
          "engine.contextual_text_literal.composed_partition_missing");
    }
    return false;
  }
  sblr::ContextualTextLiteralProfileSetV2 profiles;
  {
    std::lock_guard<std::mutex> guard(authority_handle.authority_->mutex);
    if (authority_handle.authority_->state !=
            EngineContextualTextLiteralAuthorityStateV2::issued ||
        !SamePinnedContext(authority_handle.authority_->pinned_context,
                           context) ||
        authority_handle.authority_->composed_transfer.has_value()) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "SBLR.CONTEXTUAL_TEXT_LITERAL.BINDING_STALE",
            "engine.contextual_text_literal.composed_partition_state_stale");
      }
      return false;
    }
    try {
      profiles = authority_handle.authority_->profile_set;
    } catch (const std::bad_alloc&) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "ENGINE.RESOURCE.EXHAUSTED",
            "engine.contextual_text_literal.composed_partition_allocation_failed");
      }
      return false;
    }
  }
  const auto table = DecodeContextualTextComposedSbxnV2(
      exact_sbxn.data(), exact_sbxn.size());
  if (!table.ok || profiles.mappings.empty() ||
      profiles.mappings.size() > sblr::kContextualTextMaximumProfileCountV2 ||
      numeric_node_ids.size() + profiles.mappings.size() > 4096 ||
      table.table.nodes.size() !=
          numeric_node_ids.size() + profiles.mappings.size()) {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "SBLR.OPERAND_INVALID",
          "engine.contextual_text_literal.composed_partition_count_invalid",
          table.detail);
    }
    return false;
  }

  std::set<std::uint64_t> numeric_ids;
  for (const auto node_id : numeric_node_ids) {
    if (node_id == 0 || !numeric_ids.insert(node_id).second) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "SBLR.OPERAND_INVALID",
            "engine.contextual_text_literal.numeric_partition_duplicate");
      }
      return false;
    }
  }
  sblr::ContextualTextUuidV2 text_descriptor{};
  if (!ToWireUuid("019d0000-0000-7000-8000-00000000d718",
                  &text_descriptor)) {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "ENGINE.INTERNAL.ERROR",
          "engine.contextual_text_literal.text_descriptor_identity_invalid");
    }
    return false;
  }
  std::set<std::uint64_t> contextual_ids;
  std::set<std::uint64_t> contextual_occurrences;
  std::uint64_t contextual_body_bytes = 0;
  for (const auto& mapping : profiles.mappings) {
    const auto& profile = mapping.profile;
    if (mapping.node_id == 0 ||
        !contextual_ids.insert(mapping.node_id).second ||
        !contextual_occurrences.insert(mapping.literal_occurrence).second ||
        numeric_ids.contains(mapping.node_id) ||
        mapping.literal_occurrence != profile.literal_occurrence ||
        mapping.node_id != profile.node_id ||
        profile.literal_occurrence != profile.parent_operand_ordinal ||
        profile.descriptor_uuid != text_descriptor ||
        profile.descriptor_generation != 1 ||
        profile.canonical_body.size() >
            sblr::kContextualTextMaximumBodyBytesV2 ||
        sblr::ComputeContextualTextCanonicalBodySha256V2(profile) !=
            profile.canonical_body_sha256 ||
        profile.canonical_body.size() >
            std::numeric_limits<std::uint64_t>::max() -
                contextual_body_bytes) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "SBLR.OPERAND_INVALID",
            "engine.contextual_text_literal.contextual_partition_profile_invalid");
      }
      return false;
    }
    contextual_body_bytes += profile.canonical_body.size();
    const auto node = std::ranges::find_if(
        table.table.nodes, [&](const sblr::SblrExpressionLiteralNodeV1& item) {
          return item.node_id == mapping.node_id;
        });
    if (node == table.table.nodes.end() ||
        node->parent_operand_ordinal != profile.parent_operand_ordinal ||
        node->descriptor_uuid != text_descriptor ||
        node->descriptor_generation != 1 ||
        node->literal_body != profile.canonical_body) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "SBLR.OPERAND_INVALID",
            "engine.contextual_text_literal.contextual_partition_node_invalid");
      }
      return false;
    }
  }
  if (contextual_body_bytes > profiles.canonical_body_aggregate_grant) {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "SBLR.CONTEXTUAL_TEXT_LITERAL.BUDGET_EXCEEDED",
          "engine.contextual_text_literal.composed_body_grant_exceeded");
    }
    return false;
  }
  std::size_t exact_size = 32;
  for (const auto& node : table.table.nodes) {
    const bool numeric = numeric_ids.contains(node.node_id);
    const bool contextual = contextual_ids.contains(node.node_id);
    if (numeric == contextual || (numeric && node.literal_body.size() > 24) ||
        node.literal_body.size() >
            std::numeric_limits<std::size_t>::max() - 125 ||
        125 + node.literal_body.size() >
            std::numeric_limits<std::size_t>::max() - exact_size) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "SBLR.OPERAND_INVALID",
            "engine.contextual_text_literal.composed_partition_not_exhaustive");
      }
      return false;
    }
    exact_size += 125 + node.literal_body.size();
  }
  if (exact_size != exact_sbxn.size()) {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "SBLR.OPERAND_INVALID",
          "engine.contextual_text_literal.composed_partition_size_invalid");
    }
    return false;
  }
  if (diagnostic != nullptr) *diagnostic = OkDiagnostic();
  return true;
}

EngineContextualTextLiteralTransferResultV2
TransferContextualTextLiteralAuthorityV2(
    const EngineContextualTextLiteralTransferRequestV2& request) {
  EngineContextualTextLiteralTransferResultV2 result;
  if (request.authority == nullptr || !request.authority->valid() ||
      !Nonzero(request.final_receipt_uuid) ||
      !Nonzero(request.admission_token_uuid) ||
      request.final_receipt_uuid == request.admission_token_uuid ||
      !Nonzero(request.admission_token_binding_sha256) ||
      !Nonzero(request.v1_demand_sha256) ||
      !Nonzero(request.v1_ordered_profile_sha256) ||
      !Nonzero(request.bound_ast_sha256) ||
      !Nonzero(request.complete_sbxn_sha256)) {
    result.diagnostic = Diagnostic(
        "SBLR.OPERAND_INVALID",
        "engine.contextual_text_literal.transfer_request_invalid");
    return result;
  }
  const auto authority = request.authority->authority_;
  sblr::ContextualTextLiteralNegotiationRequestV2 issued_request;
  sblr::ContextualTextLiteralProfileSetV2 profile_set;
  SblrExecutorAvailabilitySnapshot availability;
  try {
    std::lock_guard<std::mutex> guard(authority->mutex);
    if (authority->state !=
            EngineContextualTextLiteralAuthorityStateV2::issued ||
        authority->composed_transfer.has_value() ||
        !SamePinnedContext(authority->pinned_context, request.context)) {
      result.diagnostic = Diagnostic(
          "SBLR.CONTEXTUAL_TEXT_LITERAL.BINDING_STALE",
          "engine.contextual_text_literal.transfer_state_stale");
      return result;
    }
    issued_request = authority->request;
    profile_set = authority->profile_set;
    availability = authority->executor_availability;
  } catch (const std::bad_alloc&) {
    result.diagnostic = Diagnostic(
        "ENGINE.RESOURCE.EXHAUSTED",
        "engine.contextual_text_literal.transfer_snapshot_allocation_failed");
    return result;
  }
  if (profile_set.mappings.empty() ||
      request.final_receipt_uuid == issued_request.statement_receipt_uuid ||
      request.admission_token_uuid == issued_request.statement_receipt_uuid ||
      profile_set.statement_receipt_uuid !=
          issued_request.statement_receipt_uuid ||
      profile_set.demand_sequence_sha256 !=
          issued_request.demand_sequence_sha256 ||
      !Nonzero(profile_set.carrier_sha256)) {
    result.diagnostic = Diagnostic(
        "SBLR.CONTEXTUAL_TEXT_LITERAL.BINDING_STALE",
        "engine.contextual_text_literal.transfer_profile_set_invalid");
    return result;
  }
  SblrExecutorAvailabilitySnapshot current_availability;
  const auto availability_diagnostic = RevalidateSblrExecutorAvailability(
      request.context, ContextualExecutorIdentity(), availability,
      &current_availability);
  if (availability_diagnostic.error ||
      !ValidExecutorAvailability(current_availability, request.context) ||
      !(current_availability.snapshot_uuid == availability.snapshot_uuid &&
        current_availability.generation == availability.generation &&
        current_availability.database_uuid == availability.database_uuid &&
        current_availability.row_identity_sha256 ==
            availability.row_identity_sha256 &&
        current_availability.installed == availability.installed &&
        current_availability.availability_state ==
            availability.availability_state &&
        current_availability.decision_evidence_sha256 ==
            availability.decision_evidence_sha256)) {
    result.diagnostic = availability_diagnostic.error
                            ? availability_diagnostic
                            : Diagnostic(
                                  "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
                                  "engine.contextual_text_literal.transfer_executor_stale");
    return result;
  }

  using SourceKey =
      std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>;
  struct SourceIdentity {
    sblr::ContextualTextUuidV2 uuid{};
    std::uint64_t generation = 0;
  };
  std::map<SourceKey, SourceIdentity> source_identities;
  if (issued_request.demands.size() != profile_set.mappings.size()) {
    result.diagnostic = Diagnostic(
        "SBLR.CONTEXTUAL_TEXT_LITERAL.BINDING_STALE",
        "engine.contextual_text_literal.transfer_source_set_invalid");
    return result;
  }
  for (std::size_t index = 0; index != issued_request.demands.size(); ++index) {
    const auto& demand = issued_request.demands[index];
    const auto& mapping = profile_set.mappings[index];
    const auto& profile = mapping.profile;
    if (mapping.literal_occurrence != demand.literal_occurrence ||
        mapping.node_id != demand.node_id ||
        !Nonzero(profile.source_occurrence_uuid) ||
        profile.source_generation == 0) {
      result.diagnostic = Diagnostic(
          "SBLR.CONTEXTUAL_TEXT_LITERAL.BINDING_STALE",
          "engine.contextual_text_literal.transfer_source_mapping_invalid");
      return result;
    }
    const SourceKey key{demand.source_node_id,
                        demand.source_operand_ordinal,
                        demand.source_ordinal};
    const SourceIdentity identity{profile.source_occurrence_uuid,
                                  profile.source_generation};
    const auto [iterator, inserted] = source_identities.emplace(key, identity);
    if (!inserted &&
        (iterator->second.uuid != identity.uuid ||
         iterator->second.generation != identity.generation)) {
      result.diagnostic = Diagnostic(
          "SBLR.CONTEXTUAL_TEXT_LITERAL.BINDING_STALE",
          "engine.contextual_text_literal.transfer_source_identity_splice");
      return result;
    }
  }
  if (source_identities.empty() ||
      source_identities.size() > std::numeric_limits<std::uint32_t>::max()) {
    result.diagnostic = Diagnostic(
        "SBLR.CONTEXTUAL_TEXT_LITERAL.BINDING_STALE",
        "engine.contextual_text_literal.transfer_source_count_invalid");
    return result;
  }

  sblr::ContextualTextUuidV2 capability_uuid{};
  sblr::ContextualTextUuidV2 evidence_format_uuid{};
  sblr::ContextualTextUuidV2 availability_snapshot_uuid{};
  sblr::ContextualTextUuidV2 availability_database_uuid{};
  sblr::ContextualTextSha256V2 row_identity_sha256{};
  sblr::ContextualTextSha256V2 decision_evidence_sha256{};
  if (!ToWireUuid("098229e6-00f4-53ee-89da-452f2f0767c2",
                  &capability_uuid) ||
      !ToWireUuid("24bc744a-95d3-5668-b32e-aaf4616bdb4c",
                  &evidence_format_uuid) ||
      !ToWireUuid(availability.snapshot_uuid, &availability_snapshot_uuid) ||
      !ToWireUuid(availability.database_uuid, &availability_database_uuid) ||
      !DecodeStoredSha256(availability.row_identity_sha256,
                          &row_identity_sha256) ||
      !DecodeStoredSha256(availability.decision_evidence_sha256,
                          &decision_evidence_sha256)) {
    result.diagnostic = Diagnostic(
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
        "engine.contextual_text_literal.transfer_executor_encoding_invalid");
    return result;
  }

  EngineContextualTextComposedTransferRecordV2 record;
  record.final_receipt_uuid = request.final_receipt_uuid;
  record.admission_token_uuid = request.admission_token_uuid;
  record.preliminary_receipt_uuid = issued_request.statement_receipt_uuid;
  record.profile_set_uuid = profile_set.profile_set_uuid;
  record.profile_set_generation = profile_set.profile_set_generation;
  record.admission_token_binding_sha256 =
      request.admission_token_binding_sha256;
  record.bound_ast_sha256 = request.bound_ast_sha256;
  record.complete_sbxn_sha256 = request.complete_sbxn_sha256;
  try {
    constexpr std::string_view kDomain =
        "ScratchBird.ContextualText.ComposedLiteralAdmission.V2";
    auto& material = record.exact_evidence_material;
    material.reserve(512 + source_identities.size() * 24);
    material.insert(material.end(), kDomain.begin(), kDomain.end());
    material.push_back(0);
    AppendUuid(&material, record.final_receipt_uuid);
    AppendUuid(&material, record.admission_token_uuid);
    AppendUuid(&material, record.preliminary_receipt_uuid);
    AppendUuid(&material, record.profile_set_uuid);
    AppendU64(&material, record.profile_set_generation);
    AppendSha(&material, profile_set.demand_sequence_sha256);
    AppendSha(&material, profile_set.target_context_sequence_sha256);
    AppendSha(&material, profile_set.ordered_profiles_sha256);
    AppendSha(&material, profile_set.carrier_sha256);
    AppendSha(&material, request.v1_demand_sha256);
    AppendSha(&material, request.v1_ordered_profile_sha256);
    AppendSha(&material, request.bound_ast_sha256);
    AppendSha(&material, request.complete_sbxn_sha256);
    AppendUuid(&material, profile_set.catalog_snapshot_uuid);
    AppendU64(&material, profile_set.catalog_generation);
    AppendU64(&material, profile_set.datatype_registry_generation);
    AppendU64(&material, profile_set.security_generation);
    AppendU64(&material, profile_set.resource_epoch);
    AppendUuid(&material, profile_set.mga_snapshot_uuid);
    AppendUuid(&material, profile_set.literal_budget_uuid);
    AppendU64(&material, profile_set.literal_budget_generation);
    AppendU64(&material, profile_set.literal_negotiation_byte_grant);
    AppendU64(&material, profile_set.canonical_body_aggregate_grant);
    AppendU32(&material,
              static_cast<std::uint32_t>(source_identities.size()));
    for (const auto& [key, identity] : source_identities) {
      (void)key;
      AppendUuid(&material, identity.uuid);
      AppendU64(&material, identity.generation);
    }
    AppendUuid(&material, capability_uuid);
    AppendU64(&material, 1);
    AppendUuid(&material, evidence_format_uuid);
    AppendU64(&material, 1);
    AppendUuid(&material, availability_snapshot_uuid);
    AppendU64(&material, availability.generation);
    AppendUuid(&material, availability_database_uuid);
    AppendSha(&material, row_identity_sha256);
    material.push_back(1);
    material.push_back(1);
    material.insert(material.end(), 6, 0);
    AppendSha(&material, decision_evidence_sha256);
    record.evidence_sha256 = HashBytes(material);
  } catch (const std::bad_alloc&) {
    result.diagnostic = Diagnostic(
        "ENGINE.RESOURCE.EXHAUSTED",
        "engine.contextual_text_literal.transfer_evidence_allocation_failed");
    return result;
  }
  if (!Nonzero(record.evidence_sha256)) {
    result.diagnostic = Diagnostic(
        "ENGINE.INTERNAL.ERROR",
        "engine.contextual_text_literal.transfer_hash_failed");
    return result;
  }

  EngineContextualTextComposedTransferRecordV2 retained_record;
  try {
    retained_record = record;
    result.record = record;
  } catch (const std::bad_alloc&) {
    result.record = {};
    result.diagnostic = Diagnostic(
        "ENGINE.RESOURCE.EXHAUSTED",
        "engine.contextual_text_literal.transfer_retention_allocation_failed");
    return result;
  }
  EngineApiDiagnostic success_diagnostic;
  try {
    success_diagnostic = OkDiagnostic();
  } catch (const std::bad_alloc&) {
    result.diagnostic = Diagnostic(
        "ENGINE.RESOURCE.EXHAUSTED",
        "engine.contextual_text_literal.success_publication_allocation_failed");
    return result;
  }
  {
    std::lock_guard<std::mutex> guard(authority->mutex);
    if (authority->state !=
            EngineContextualTextLiteralAuthorityStateV2::issued ||
        authority->composed_transfer.has_value() ||
        !SamePinnedContext(authority->pinned_context, request.context)) {
      result.record = {};
      result.diagnostic = Diagnostic(
          "SBLR.CONTEXTUAL_TEXT_LITERAL.REPLAY",
          "engine.contextual_text_literal.transfer_publication_lost");
      return result;
    }
    authority->composed_transfer.emplace(std::move(retained_record));
  }
  result.ok = true;
  result.diagnostic = std::move(success_diagnostic);
  return result;
}

EngineContextualTextLiteralAuthorityPrepareResultV2
PrepareContextualTextLiteralAuthorityV2(
    const EngineContextualTextLiteralAuthorityPrepareRequestV2& request) {
  EngineContextualTextLiteralAuthorityPrepareResultV2 result;
  if (request.authority == nullptr || !request.authority->valid() ||
      request.graph_verifier == nullptr || request.target_resolver == nullptr ||
      request.exact_sbel_v1.empty() || request.exact_canonical_sbos.empty() ||
      request.composed_transfer == nullptr ||
      request.exact_sbtlxe02.empty() ||
      request.exact_pre_contextual_operand_records.empty() ||
      request.pre_contextual_operand_count == 0 ||
      request.exact_sbxn.empty()) {
    result.diagnostic = Diagnostic(
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
        "engine.contextual_text_literal.prepare_authority_missing");
    return result;
  }
  if (request.context.query_cancellation_requested &&
      request.context.query_cancellation_requested()) {
    result.diagnostic = Diagnostic(
        "PROCESS.CANCELLED", "engine.contextual_text_literal.cancelled");
    return result;
  }

  const auto authority = request.authority->authority_;
  sblr::ContextualTextLiteralNegotiationRequestV2 issued_request;
  sblr::ContextualTextLiteralProfileSetV2 issued_profile_set;
  std::vector<EngineResolvedContextualTextTargetV2> targets;
  SblrExecutorAvailabilitySnapshot availability;
  EngineContextualTextComposedTransferRecordV2 retained_transfer;
  const EngineContextualTextTargetAuthorityResolverV2* issued_target_resolver =
      nullptr;
  try {
    std::lock_guard<std::mutex> guard(authority->mutex);
    if (authority->state !=
        EngineContextualTextLiteralAuthorityStateV2::issued) {
      result.diagnostic = Diagnostic(
          authority->state ==
                  EngineContextualTextLiteralAuthorityStateV2::revoked
              ? "SBLR.CONTEXTUAL_TEXT_LITERAL.BINDING_STALE"
              : "SBLR.CONTEXTUAL_TEXT_LITERAL.REPLAY",
          "engine.contextual_text_literal.prepare_not_issued");
      return result;
    }
    if (!SamePinnedContext(authority->pinned_context, request.context)) {
      result.diagnostic = Diagnostic(
          "SBLR.CONTEXTUAL_TEXT_LITERAL.BINDING_STALE",
          "engine.contextual_text_literal.prepare_context_stale");
      return result;
    }
    issued_request = authority->request;
    issued_profile_set = authority->profile_set;
    targets = authority->targets;
    availability = authority->executor_availability;
    if (!authority->composed_transfer.has_value()) {
      result.diagnostic = Diagnostic(
          "SBLR.CONTEXTUAL_TEXT_LITERAL.BINDING_STALE",
          "engine.contextual_text_literal.transfer_not_finalized");
      return result;
    }
    retained_transfer = *authority->composed_transfer;
    issued_target_resolver = authority->target_resolver;
  } catch (const std::bad_alloc&) {
    result.diagnostic = Diagnostic(
        "ENGINE.RESOURCE.EXHAUSTED",
        "engine.contextual_text_literal.prepare_snapshot_allocation_failed");
    return result;
  }
  if (request.target_resolver != issued_target_resolver) {
    result.diagnostic = Diagnostic(
        "SECURITY.ACCESS_DENIED",
        "engine.contextual_text_literal.resolver_substitution_refused");
    return result;
  }
  if (retained_transfer != *request.composed_transfer) {
    result.diagnostic = Diagnostic(
        "ENGINE.STATEMENT_CONTEXT.ADMISSION_TOKEN_STALE",
        "engine.contextual_text_literal.transfer_receipt_mismatch");
    return result;
  }
  const auto exact_sbos_sha256 = HashBytes(request.exact_canonical_sbos);
  if (!Nonzero(exact_sbos_sha256) ||
      !ExactSbelMatchesTransfer(request.exact_sbel_v1, retained_transfer,
                                &exact_sbos_sha256)) {
    result.diagnostic = Diagnostic(
        "ENGINE.STATEMENT_CONTEXT.ADMISSION_TOKEN_STALE",
        "engine.contextual_text_literal.sbel_transfer_mismatch");
    return result;
  }

  sblr::ContextualTextLiteralExecuteV2 execute;
  sblr::ContextualTextCodecDiagnosticV2 codec_diagnostic;
  if (!sblr::DecodeContextualTextLiteralExecuteV2(
          request.exact_sbtlxe02.data(), request.exact_sbtlxe02.size(),
          &execute, &codec_diagnostic)) {
    result.diagnostic = CodecDiagnostic(codec_diagnostic);
    return result;
  }
  const auto same_common_header =
      execute.statement_receipt_uuid ==
          issued_profile_set.statement_receipt_uuid &&
      execute.profile_set_uuid == issued_profile_set.profile_set_uuid &&
      execute.profile_set_generation ==
          issued_profile_set.profile_set_generation &&
      execute.catalog_snapshot_uuid ==
          issued_profile_set.catalog_snapshot_uuid &&
      execute.catalog_generation == issued_profile_set.catalog_generation &&
      execute.datatype_registry_generation ==
          issued_profile_set.datatype_registry_generation &&
      execute.security_generation == issued_profile_set.security_generation &&
      execute.resource_epoch == issued_profile_set.resource_epoch &&
      execute.mga_snapshot_uuid == issued_profile_set.mga_snapshot_uuid &&
      execute.literal_budget_uuid == issued_profile_set.literal_budget_uuid &&
      execute.literal_budget_generation ==
          issued_profile_set.literal_budget_generation &&
      execute.literal_negotiation_byte_grant ==
          issued_profile_set.literal_negotiation_byte_grant &&
      execute.canonical_body_aggregate_grant ==
          issued_profile_set.canonical_body_aggregate_grant &&
      execute.demand_sequence_sha256 ==
          issued_profile_set.demand_sequence_sha256 &&
      execute.target_context_sequence_sha256 ==
          issued_profile_set.target_context_sequence_sha256 &&
      execute.ordered_profiles_sha256 ==
          issued_profile_set.ordered_profiles_sha256 &&
      execute.mappings.size() == issued_profile_set.mappings.size();
  if (!same_common_header) {
    result.diagnostic = Diagnostic(
        "SBLR.CONTEXTUAL_TEXT_LITERAL.BINDING_STALE",
        "engine.contextual_text_literal.execute_profile_set_mismatch");
    return result;
  }
  for (std::size_t index = 0; index != execute.mappings.size(); ++index) {
    const auto& actual = execute.mappings[index];
    const auto& issued = issued_profile_set.mappings[index];
    if (actual.literal_occurrence != issued.literal_occurrence ||
        actual.node_id != issued.node_id ||
        actual.literal_binding_uuid != issued.literal_binding_uuid ||
        actual.literal_binding_generation !=
            issued.literal_binding_generation ||
        actual.literal_descriptor_handle !=
            issued.literal_descriptor_handle ||
        actual.target_descriptor_handle != issued.target_descriptor_handle ||
        actual.profile.exact_bytes != issued.profile.exact_bytes) {
      result.diagnostic = Diagnostic(
          "SBLR.CONTEXTUAL_TEXT_LITERAL.NON_CANONICAL",
          "engine.contextual_text_literal.profile_splice_refused");
      return result;
    }
  }
  if (execute.pre_contextual_operand_vector_sha256 !=
          sblr::ComputeContextualTextPreContextualOperandVectorSha256V2(
              request.exact_pre_contextual_operand_records,
              request.pre_contextual_operand_count) ||
      execute.sbxn_sha256 !=
          sblr::ComputeContextualTextSbxnSha256V2(request.exact_sbxn) ||
      execute.sbxn_sha256 != retained_transfer.complete_sbxn_sha256) {
    result.diagnostic = Diagnostic(
        "SBLR.CONTEXTUAL_TEXT_LITERAL.NON_CANONICAL",
        "engine.contextual_text_literal.execute_evidence_hash_mismatch");
    return result;
  }
  if (targets.size() != issued_request.demands.size()) {
    result.diagnostic = Diagnostic(
        "SBLR.CONTEXTUAL_TEXT_LITERAL.BINDING_STALE",
        "engine.contextual_text_literal.target_set_incomplete");
    return result;
  }
  EngineApiDiagnostic resolver_diagnostic;
  for (std::size_t index = 0; index != targets.size(); ++index) {
    if (!request.target_resolver->RevalidateTarget(
            request.context, issued_request.demands[index], targets[index],
            &resolver_diagnostic)) {
      result.diagnostic = std::move(resolver_diagnostic);
      return result;
    }
  }
  SblrExecutorAvailabilitySnapshot current_availability;
  const auto availability_diagnostic = RevalidateSblrExecutorAvailability(
      request.context, ContextualExecutorIdentity(), availability,
      &current_availability);
  if (availability_diagnostic.error) {
    result.diagnostic = availability_diagnostic;
    return result;
  }
  std::vector<EngineContextualTextVerifiedGraphBindingV2>
      verified_graph_bindings;
  if (!request.graph_verifier->VerifyPrepareEvidence(
          request.context, issued_request, execute, request.exact_sbel_v1,
          request.exact_canonical_sbos, retained_transfer,
          request.exact_pre_contextual_operand_records,
          request.pre_contextual_operand_count, request.exact_sbxn,
          &verified_graph_bindings,
          &resolver_diagnostic)) {
    result.diagnostic = std::move(resolver_diagnostic);
    return result;
  }
  if (verified_graph_bindings.size() != execute.mappings.size()) {
    result.diagnostic = Diagnostic(
        "SBLR.OPERAND_INVALID",
        "engine.contextual_text_literal.verified_graph_binding_count_mismatch");
    return result;
  }

  std::unique_ptr<PreparedContextualTextLiteralSetV2::State> prepared;
  try {
    prepared = std::make_unique<PreparedContextualTextLiteralSetV2::State>();
    prepared->authority = authority;
    prepared->exact_sbel_v1 = request.exact_sbel_v1;
    prepared->composed_transfer = retained_transfer;
    prepared->sbos_sha256 = exact_sbos_sha256;
    prepared->pre_contextual_operand_vector_sha256 =
        execute.pre_contextual_operand_vector_sha256;
    prepared->sbxn_sha256 = HashBytes(request.exact_sbxn);
    prepared->sbtlxe_sha256 = HashBytes(request.exact_sbtlxe02);
    prepared->entries.reserve(execute.mappings.size());
    for (std::size_t index = 0; index != execute.mappings.size(); ++index) {
      const auto& mapping = execute.mappings[index];
      const auto& demand = issued_request.demands[index];
      const auto& graph_binding = verified_graph_bindings[index];
      const auto expected_literal_descriptor =
          ExpectedLiteralDescriptorFields(mapping.profile);
      auto expected_target_descriptor = expected_literal_descriptor;
      if (graph_binding.exact_target_relational_descriptor_v2_fields[7] ==
              "0" ||
          graph_binding.exact_target_relational_descriptor_v2_fields[7] ==
              "1") {
        expected_target_descriptor[7] =
            graph_binding.exact_target_relational_descriptor_v2_fields[7];
      }
      if (graph_binding.literal_occurrence != mapping.literal_occurrence ||
          graph_binding.node_id != mapping.node_id ||
          graph_binding.literal_expression_id == 0 ||
          graph_binding.comparison_expression_id == 0 ||
          graph_binding.target_expression_id == 0 ||
          graph_binding.source_node_id == 0 ||
          graph_binding.literal_expression_id ==
              graph_binding.comparison_expression_id ||
          graph_binding.literal_expression_id ==
              graph_binding.target_expression_id ||
          graph_binding.comparison_expression_id ==
              graph_binding.target_expression_id ||
          graph_binding.comparison_expression_id !=
              mapping.profile.comparison_occurrence ||
          graph_binding.source_node_id != demand.source_node_id ||
          graph_binding.literal_descriptor_handle !=
              mapping.literal_descriptor_handle ||
          graph_binding.target_descriptor_handle !=
              mapping.target_descriptor_handle ||
          graph_binding.canonical_type_name != "text" ||
          !graph_binding.element_profile_empty ||
          graph_binding.exact_relational_descriptor_v2_fields !=
              expected_literal_descriptor ||
          graph_binding.target_canonical_type_name != "text" ||
          !graph_binding.target_element_profile_empty ||
          graph_binding.exact_target_relational_descriptor_v2_fields !=
              expected_target_descriptor) {
        result.diagnostic = Diagnostic(
            "SBLR.OPERAND_INVALID",
            "engine.contextual_text_literal.verified_graph_binding_mismatch");
        return result;
      }
      EngineContextualTextComparisonResourceSnapshotV2
          comparison_resources;
      if (!ResolveComparisonResources(
              request.context, *request.target_resolver, demand,
              mapping.profile, targets[index], &comparison_resources,
              &resolver_diagnostic)) {
        result.diagnostic = std::move(resolver_diagnostic);
        return result;
      }
      EngineDescriptor projected_target_descriptor;
      if (!ResolveProjectedTargetDescriptor(
              demand, mapping.profile, targets[index],
              graph_binding.exact_target_relational_descriptor_v2_fields,
              &projected_target_descriptor, &resolver_diagnostic)) {
        result.diagnostic = std::move(resolver_diagnostic);
        return result;
      }
      PreparedContextualTextValueV2 value;
      value.literal_occurrence = mapping.literal_occurrence;
      value.node_id = mapping.node_id;
      value.literal_descriptor_handle = mapping.literal_descriptor_handle;
      value.descriptor_uuid = mapping.profile.descriptor_uuid;
      value.descriptor_generation = mapping.profile.descriptor_generation;
      value.type_uuid = mapping.profile.type_uuid;
      value.type_generation = mapping.profile.type_generation;
      value.codec_uuid = mapping.profile.codec_uuid;
      value.codec_id = sblr::kContextualTextCodecIdentifierV2;
      value.codec_version = mapping.profile.codec_version;
      value.codec_generation = mapping.profile.codec_generation;
      value.canonical_body_sha256 = mapping.profile.canonical_body_sha256;
      value.canonical_body = mapping.profile.canonical_body;
      value.scalar_count = mapping.profile.scalar_count;
      value.profile_set_uuid = mapping.profile.profile_set_uuid;
      value.profile_set_generation = mapping.profile.profile_set_generation;
      value.literal_binding_uuid = mapping.profile.literal_binding_uuid;
      value.literal_binding_generation =
          mapping.profile.literal_binding_generation;
      value.target_context_sha256 = mapping.profile.target_context_sha256;
      EngineContextualTextPreparedRuntimeMaterializationV2 runtime;
      runtime.graph_binding = graph_binding;
      const auto literal_descriptor = JoinLiteralDescriptorFields(
          graph_binding.exact_relational_descriptor_v2_fields);
      const auto target_descriptor = JoinLiteralDescriptorFields(
          graph_binding.exact_target_relational_descriptor_v2_fields);
      runtime.exact_literal_relational_descriptor_v2_bytes.assign(
          literal_descriptor.begin(), literal_descriptor.end());
      runtime.exact_target_relational_descriptor_v2_bytes.assign(
          target_descriptor.begin(), target_descriptor.end());
      runtime.value.descriptor.descriptor_uuid.canonical =
          graph_binding.exact_relational_descriptor_v2_fields[0];
      runtime.value.descriptor.descriptor_kind = "scalar";
      runtime.value.descriptor.canonical_type_name = "text";
      runtime.value.descriptor.encoded_descriptor = RuntimeEncodedDescriptor(
          graph_binding.exact_relational_descriptor_v2_fields,
          mapping.profile, literal_descriptor);
      runtime.value.encoded_value.assign(mapping.profile.canonical_body.begin(),
                                         mapping.profile.canonical_body.end());
      runtime.value.binary_value.clear();
      runtime.value.is_null = false;
      runtime.value.state = EngineValueState::value;
      runtime.target_descriptor = std::move(projected_target_descriptor);
      runtime.exact_profile = mapping.profile;
      runtime.comparison_resources = std::move(comparison_resources);

      EngineContextualTextPreparedExecutionEntryV2 entry;
      entry.prepared_value = std::move(value);
      entry.runtime_materialization = std::move(runtime);
      entry.comparison_occurrence =
          graph_binding.comparison_expression_id;
      entry.target_descriptor_handle = mapping.target_descriptor_handle;
      entry.literal_argument_ordinal =
          mapping.profile.literal_argument_ordinal;
      entry.target_argument_ordinal =
          mapping.profile.target_argument_ordinal;
      entry.equality_operation_uuid =
          mapping.profile.equality_operation_uuid;
      entry.equality_operation_generation =
          mapping.profile.equality_operation_generation;
      prepared->entries.push_back(std::move(entry));
    }
  } catch (const std::bad_alloc&) {
    result.diagnostic = Diagnostic(
        "ENGINE.RESOURCE.EXHAUSTED",
        "engine.contextual_text_literal.prepare_allocation_failed");
    return result;
  }
  EngineApiDiagnostic success_diagnostic;
  try {
    success_diagnostic = OkDiagnostic();
  } catch (const std::bad_alloc&) {
    result.diagnostic = Diagnostic(
        "ENGINE.RESOURCE.EXHAUSTED",
        "engine.contextual_text_literal.success_publication_allocation_failed");
    return result;
  }
  {
    std::lock_guard<std::mutex> guard(authority->mutex);
    if (authority->state !=
            EngineContextualTextLiteralAuthorityStateV2::issued ||
        !SamePinnedContext(authority->pinned_context, request.context)) {
      result.diagnostic = Diagnostic(
          "SBLR.CONTEXTUAL_TEXT_LITERAL.BINDING_STALE",
          "engine.contextual_text_literal.prepare_raced_invalidation");
      return result;
    }
  }
  result.prepared.state_ = std::move(prepared);
  result.ok = true;
  result.diagnostic = std::move(success_diagnostic);
  return result;
}

EngineContextualTextLiteralJointConsumeResultV2
JointConsumeContextualTextLiteralAuthorityV2(
    const EngineContextualTextLiteralJointConsumeRequestV2& request) {
  EngineContextualTextLiteralJointConsumeResultV2 result;
  if (request.context == nullptr || request.exact_sbel_v1 == nullptr ||
      request.authority == nullptr || !request.authority->valid() ||
      request.prepared == nullptr || !request.prepared->valid() ||
      request.composed_transfer == nullptr ||
      request.receipt_literal_admission_consumed == nullptr) {
    result.diagnostic = Diagnostic(
        "ENGINE.STATEMENT_CONTEXT.ADMISSION_TOKEN_STALE",
        "engine.contextual_text_literal.joint_pair_missing");
    return result;
  }
  auto& prepared = *request.prepared->state_;
  const auto authority = request.authority->authority_;
  if (prepared.authority != authority ||
      prepared.entries.empty() ||
      prepared.exact_sbel_v1 != *request.exact_sbel_v1 ||
      prepared.composed_transfer != *request.composed_transfer ||
      !ExactSbelMatchesTransfer(*request.exact_sbel_v1,
                                *request.composed_transfer, nullptr) ||
      *request.receipt_literal_admission_consumed) {
    result.diagnostic = Diagnostic(
        "ENGINE.STATEMENT_CONTEXT.ADMISSION_TOKEN_STALE",
        "engine.contextual_text_literal.joint_evidence_mismatch");
    return result;
  }
  if (request.context->query_cancellation_requested &&
      request.context->query_cancellation_requested()) {
    result.diagnostic = Diagnostic(
        "PROCESS.CANCELLED", "engine.contextual_text_literal.cancelled");
    return result;
  }

  SblrExecutorAvailabilitySnapshot availability;
  sblr::ContextualTextLiteralNegotiationRequestV2 issued_request;
  std::vector<EngineResolvedContextualTextTargetV2> targets;
  const EngineContextualTextTargetAuthorityResolverV2* retained_target_resolver =
      nullptr;
  try {
    std::lock_guard<std::mutex> guard(authority->mutex);
    if (authority->state !=
        EngineContextualTextLiteralAuthorityStateV2::issued) {
      result.diagnostic = Diagnostic(
          authority->state ==
                  EngineContextualTextLiteralAuthorityStateV2::revoked
              ? "ENGINE.STATEMENT_CONTEXT.ADMISSION_TOKEN_STALE"
              : "SBLR.CONTEXTUAL_TEXT_LITERAL.REPLAY",
          "engine.contextual_text_literal.joint_pair_not_ready");
      return result;
    }
    if (!SamePinnedContext(authority->pinned_context, *request.context)) {
      result.diagnostic = Diagnostic(
          "ENGINE.STATEMENT_CONTEXT.ADMISSION_TOKEN_STALE",
          "engine.contextual_text_literal.joint_context_stale");
      return result;
    }
    if (!authority->composed_transfer.has_value() ||
        *authority->composed_transfer != *request.composed_transfer) {
      result.diagnostic = Diagnostic(
          "ENGINE.STATEMENT_CONTEXT.ADMISSION_TOKEN_STALE",
          "engine.contextual_text_literal.joint_transfer_stale");
      return result;
    }
    availability = authority->executor_availability;
    issued_request = authority->request;
    targets = authority->targets;
    retained_target_resolver = authority->target_resolver;
  } catch (const std::bad_alloc&) {
    result.diagnostic = Diagnostic(
        "ENGINE.RESOURCE.EXHAUSTED",
        "engine.contextual_text_literal.joint_snapshot_allocation_failed");
    return result;
  }
  // Allocate the lease owner before the non-failing transition.  The vectors
  // themselves transfer by noexcept move only after the winning state change.
  std::unique_ptr<ContextualTextExecutionAuthorityLeaseV2::State> lease_state(
      new (std::nothrow) ContextualTextExecutionAuthorityLeaseV2::State());
  if (!lease_state) {
    result.diagnostic = Diagnostic(
        "ENGINE.RESOURCE.EXHAUSTED",
        "engine.contextual_text_literal.lease_allocation_failed");
    return result;
  }
  try {
    lease_state->entries.reserve(prepared.entries.size());
  } catch (const std::bad_alloc&) {
    result.diagnostic = Diagnostic(
        "ENGINE.RESOURCE.EXHAUSTED",
        "engine.contextual_text_literal.lease_entries_allocation_failed");
    return result;
  }

  SblrExecutorAvailabilitySnapshot current_availability;
  const auto availability_diagnostic = RevalidateSblrExecutorAvailability(
      *request.context, ContextualExecutorIdentity(), availability,
      &current_availability);
  if (availability_diagnostic.error) {
    result.diagnostic = availability_diagnostic;
    return result;
  }

  // This is the final failure-capable authority lookup before the joint
  // transition.  Resolve through the exact engine-owned resolver retained at
  // issue; never accept a caller replacement or the prepared target bytes as
  // their own current-resource oracle.
  if (retained_target_resolver == nullptr ||
      targets.size() != issued_request.demands.size() ||
      targets.size() != prepared.entries.size()) {
    result.diagnostic = Diagnostic(
        "SBLR.CONTEXTUAL_TEXT_LITERAL.BINDING_STALE",
        "engine.contextual_text_literal.joint_target_set_stale");
    return result;
  }
  EngineApiDiagnostic target_diagnostic;
  for (std::size_t index = 0; index != targets.size(); ++index) {
    if (!retained_target_resolver->RevalidateTarget(
            *request.context, issued_request.demands[index], targets[index],
            &target_diagnostic)) {
      result.diagnostic = std::move(target_diagnostic);
      return result;
    }
    EngineContextualTextComparisonResourceSnapshotV2 current_resources;
    try {
      const auto& staged =
          prepared.entries[index].runtime_materialization;
      if (!ResolveComparisonResources(
              *request.context, *retained_target_resolver,
              issued_request.demands[index], staged.exact_profile,
              targets[index], &current_resources, &target_diagnostic)) {
        result.diagnostic = std::move(target_diagnostic);
        return result;
      }
      if (!SameComparisonResources(staged.comparison_resources,
                                   current_resources)) {
        result.diagnostic = Diagnostic(
            "CTB.TEXT.RESOURCE_EPOCH_MISMATCH",
            "engine.contextual_text_literal.joint_comparison_resource_stale");
        return result;
      }
    } catch (const std::bad_alloc&) {
      result.diagnostic = Diagnostic(
          "ENGINE.RESOURCE.EXHAUSTED",
          "engine.contextual_text_literal.joint_resource_allocation_failed");
      return result;
    }
  }
  if (request.context->query_cancellation_requested &&
      request.context->query_cancellation_requested()) {
    result.diagnostic = Diagnostic(
        "PROCESS.CANCELLED", "engine.contextual_text_literal.cancelled");
    return result;
  }
  EngineApiDiagnostic joint_success_diagnostic;
  try {
    joint_success_diagnostic = OkDiagnostic();
  } catch (const std::bad_alloc&) {
    result.diagnostic = Diagnostic(
        "ENGINE.RESOURCE.EXHAUSTED",
        "engine.contextual_text_literal.success_publication_allocation_failed");
    return result;
  }
  {
    std::lock_guard<std::mutex> guard(authority->mutex);
    if (authority->state !=
            EngineContextualTextLiteralAuthorityStateV2::issued ||
        !SamePinnedContext(authority->pinned_context, *request.context) ||
        authority->target_resolver != retained_target_resolver ||
        !authority->composed_transfer.has_value() ||
        *authority->composed_transfer != *request.composed_transfer) {
      result.diagnostic = Diagnostic(
          authority->state ==
                  EngineContextualTextLiteralAuthorityStateV2::consumed_with_lease
              ? "SBLR.CONTEXTUAL_TEXT_LITERAL.REPLAY"
              : "ENGINE.STATEMENT_CONTEXT.ADMISSION_TOKEN_STALE",
          "engine.contextual_text_literal.joint_transition_lost");
      return result;
    }
    if (*request.receipt_literal_admission_consumed) {
      result.diagnostic = Diagnostic(
          "SBLR.CONTEXTUAL_TEXT_LITERAL.REPLAY",
          "engine.contextual_text_literal.joint_token_already_consumed");
      return result;
    }
    authority->state =
        EngineContextualTextLiteralAuthorityStateV2::consumed_with_lease;
    *request.receipt_literal_admission_consumed = true;
    lease_state->authority = authority;
    for (auto& staged : prepared.entries) {
      ContextualTextExecutionAuthorityEntryV2 entry;
      static_cast<PreparedContextualTextValueV2&>(entry.typed_value) =
          std::move(staged.prepared_value);
      entry.typed_value.consumed_profile_uuid =
          staged.runtime_materialization.exact_profile.profile_uuid;
      entry.runtime_materialization =
          std::move(staged.runtime_materialization);
      entry.comparison_occurrence = staged.comparison_occurrence;
      entry.target_descriptor_handle = staged.target_descriptor_handle;
      entry.literal_argument_ordinal = staged.literal_argument_ordinal;
      entry.target_argument_ordinal = staged.target_argument_ordinal;
      entry.equality_operation_uuid = staged.equality_operation_uuid;
      entry.equality_operation_generation =
          staged.equality_operation_generation;
      lease_state->entries.push_back(std::move(entry));
    }
  }
  request.prepared->state_.reset();
  result.lease.state_ = std::move(lease_state);
  result.ok = true;
  result.diagnostic = std::move(joint_success_diagnostic);
  return result;
}

bool CopyContextualTextLiteralAuthoritySnapshotV2(
    const EngineContextualTextLiteralAuthorityHandleV2& handle,
    EngineContextualTextLiteralAuthoritySnapshotV2* snapshot,
    EngineApiDiagnostic* diagnostic) {
  if (snapshot == nullptr || !handle.valid()) {
    if (diagnostic != nullptr)
      *diagnostic = Diagnostic(
          "SBLR.CONTEXTUAL_TEXT_LITERAL.BINDING_STALE",
          "engine.contextual_text_literal.snapshot_invalid");
    return false;
  }
  try {
    std::lock_guard<std::mutex> guard(handle.authority_->mutex);
    snapshot->state = handle.authority_->state;
    snapshot->pinned_context = handle.authority_->pinned_context;
    snapshot->request = handle.authority_->request;
    snapshot->profile_set = handle.authority_->profile_set;
    snapshot->exact_sbtlns02 = handle.authority_->exact_sbtlns02;
    snapshot->composed_transfer = handle.authority_->composed_transfer;
  } catch (const std::bad_alloc&) {
    if (diagnostic != nullptr)
      *diagnostic = Diagnostic(
          "ENGINE.RESOURCE.EXHAUSTED",
          "engine.contextual_text_literal.snapshot_allocation_failed");
    return false;
  }
  if (diagnostic != nullptr) *diagnostic = OkDiagnostic();
  return true;
}

EngineContextualTextPreparedResourceEstimateV2
EstimatePreparedContextualTextLiteralResourcesV2(
    const PreparedContextualTextLiteralSetV2& prepared) noexcept {
  EngineContextualTextPreparedResourceEstimateV2 result;
  if (!prepared.valid() || prepared.state_->authority == nullptr ||
      prepared.state_->entries.empty()) {
    return result;
  }
  const auto& state = *prepared.state_;
  const auto& authority = *state.authority;
  if (authority.request.demands.size() != state.entries.size() ||
      authority.targets.size() != state.entries.size()) {
    return result;
  }

  LogicalByteCounterV2 prepared_bytes;
  prepared_bytes.AddObject<PreparedContextualTextLiteralSetV2::State>();
  AddVectorStorageV2(state.entries,
                     LogicalPayloadAccountingV2::retained_capacity,
                     &prepared_bytes);
  for (const auto& entry : state.entries) {
    AddDynamicPayloadV2(entry,
                        LogicalPayloadAccountingV2::retained_capacity,
                        &prepared_bytes);
  }
  AddVectorStorageV2(state.exact_sbel_v1,
                     LogicalPayloadAccountingV2::retained_capacity,
                     &prepared_bytes);
  AddDynamicPayloadV2(state.composed_transfer,
                      LogicalPayloadAccountingV2::retained_capacity,
                      &prepared_bytes);

  LogicalByteCounterV2 post_consume_bytes;
  post_consume_bytes
      .AddObject<ContextualTextExecutionAuthorityLeaseV2::State>();
  post_consume_bytes.AddProduct(state.entries.size(),
                                sizeof(ContextualTextExecutionAuthorityEntryV2));
  for (const auto& entry : state.entries) {
    AddDynamicPayloadV2(entry,
                        LogicalPayloadAccountingV2::retained_capacity,
                        &post_consume_bytes);
  }

  LogicalByteCounterV2 joint_bytes;
  joint_bytes.AddObject<EngineContextualTextLiteralJointConsumeResultV2>();
  joint_bytes.AddObject<std::shared_ptr<
      EngineContextualTextLiteralAuthorityHandleV2::Authority>>();
  joint_bytes.AddObject<std::unique_ptr<
      ContextualTextExecutionAuthorityLeaseV2::State>>();
  joint_bytes.AddObject<ContextualTextExecutionAuthorityLeaseV2::State>();
  joint_bytes.AddProduct(state.entries.size(),
                         sizeof(ContextualTextExecutionAuthorityEntryV2));
  joint_bytes.AddObject<ContextualTextExecutionAuthorityEntryV2>();

  // Immutable authority snapshots copied before the final resolver calls.
  joint_bytes.AddObject<SblrExecutorAvailabilitySnapshot>();
  AddDynamicPayloadV2(authority.executor_availability,
                      LogicalPayloadAccountingV2::copied_size,
                      &joint_bytes);
  joint_bytes.AddObject<sblr::ContextualTextLiteralNegotiationRequestV2>();
  AddDynamicPayloadV2(authority.request,
                      LogicalPayloadAccountingV2::copied_size,
                      &joint_bytes);
  joint_bytes.AddObject<std::vector<EngineResolvedContextualTextTargetV2>>();
  AddVectorStorageV2(authority.targets,
                     LogicalPayloadAccountingV2::copied_size, &joint_bytes);
  for (const auto& target : authority.targets) {
    AddDynamicPayloadV2(target, LogicalPayloadAccountingV2::copied_size,
                        &joint_bytes);
  }

  // Revalidation retains a second availability snapshot and three success
  // diagnostics until publication completes. Their exact success payloads
  // are fixed by the called registry/provider helpers.
  joint_bytes.AddObject<SblrExecutorAvailabilitySnapshot>();
  AddDynamicPayloadV2(authority.executor_availability,
                      LogicalPayloadAccountingV2::copied_size,
                      &joint_bytes);
  joint_bytes.AddProduct(3, sizeof(EngineApiDiagnostic));
  AddDiagnosticLiteralPayloadV2("OK", "ok", {}, &joint_bytes);
  AddDiagnosticLiteralPayloadV2("SB_ENGINE_API_OK", "engine.api.ok", {},
                                &joint_bytes);
  AddDiagnosticLiteralPayloadV2("SB_ENGINE_API_OK", "engine.api.ok", {},
                                &joint_bytes);

  std::uint64_t maximum_iteration_bytes = 0;
  for (std::size_t index = 0; index != state.entries.size(); ++index) {
    bool iteration_ok = false;
    const auto iteration_bytes = FinalRevalidationIterationLogicalBytesV2(
        state.entries[index], authority.targets[index], &iteration_ok);
    if (!iteration_ok) {
      result.status =
          EngineContextualTextResourceEstimateStatusV2::arithmetic_overflow;
      return result;
    }
    maximum_iteration_bytes =
        std::max(maximum_iteration_bytes, iteration_bytes);
  }
  joint_bytes.Add(maximum_iteration_bytes);

  if (!prepared_bytes.ok() || !joint_bytes.ok() ||
      !post_consume_bytes.ok()) {
    result.status =
        EngineContextualTextResourceEstimateStatusV2::arithmetic_overflow;
    return result;
  }
  result.ok = true;
  result.status = EngineContextualTextResourceEstimateStatusV2::ok;
  result.prepared_retained_bytes = prepared_bytes.value();
  result.joint_incremental_peak_bytes = joint_bytes.value();
  result.post_consume_lease_retained_bytes = post_consume_bytes.value();
  return result;
}

EngineContextualTextLeaseResourceEstimateV2
EstimateContextualTextExecutionAuthorityLeaseRetainedBytesV2(
    const ContextualTextExecutionAuthorityLeaseV2& lease) noexcept {
  EngineContextualTextLeaseResourceEstimateV2 result;
  if (!lease.valid() || lease.state_->entries.empty()) return result;
  LogicalByteCounterV2 retained_bytes;
  retained_bytes
      .AddObject<ContextualTextExecutionAuthorityLeaseV2::State>();
  AddVectorStorageV2(lease.state_->entries,
                     LogicalPayloadAccountingV2::retained_capacity,
                     &retained_bytes);
  for (const auto& entry : lease.state_->entries) {
    AddDynamicPayloadV2(entry,
                        LogicalPayloadAccountingV2::retained_capacity,
                        &retained_bytes);
  }
  if (!retained_bytes.ok()) {
    result.status =
        EngineContextualTextResourceEstimateStatusV2::arithmetic_overflow;
    return result;
  }
  result.ok = true;
  result.status = EngineContextualTextResourceEstimateStatusV2::ok;
  result.post_consume_lease_retained_bytes = retained_bytes.value();
  return result;
}

std::span<const EngineContextualTextPreparedExecutionEntryV2>
ViewPreparedContextualTextLiteralSetV2(
    const PreparedContextualTextLiteralSetV2& prepared) noexcept {
  if (!prepared.valid()) return {};
  return prepared.state_->entries;
}

const EngineContextualTextPreparedExecutionEntryV2*
FindPreparedContextualTextExecutionEntryV2(
    const PreparedContextualTextLiteralSetV2& prepared,
    const std::uint64_t literal_occurrence,
    const std::uint64_t node_id,
    const std::uint32_t literal_expression_id,
    const std::uint32_t comparison_expression_id,
    const std::uint32_t target_expression_id,
    const std::uint32_t source_node_id,
    const std::uint32_t literal_descriptor_handle,
    const std::uint32_t target_descriptor_handle,
    const std::uint8_t literal_argument_ordinal,
    const std::uint8_t target_argument_ordinal,
    const sblr::ContextualTextUuidV2& equality_operation_uuid,
    const std::uint64_t equality_operation_generation) noexcept {
  if (!prepared.valid() || literal_occurrence == 0 || node_id == 0 ||
      literal_expression_id == 0 || comparison_expression_id == 0 ||
      target_expression_id == 0 || source_node_id == 0 ||
      literal_descriptor_handle == 0 || target_descriptor_handle == 0 ||
      (literal_argument_ordinal != 1 && literal_argument_ordinal != 2) ||
      (target_argument_ordinal != 1 && target_argument_ordinal != 2) ||
      literal_argument_ordinal == target_argument_ordinal ||
      !Nonzero(equality_operation_uuid) ||
      equality_operation_generation == 0) {
    return nullptr;
  }
  const EngineContextualTextPreparedExecutionEntryV2* found = nullptr;
  for (const auto& entry : prepared.state_->entries) {
    const auto& value = entry.prepared_value;
    const auto& binding = entry.runtime_materialization.graph_binding;
    if (value.literal_occurrence != literal_occurrence ||
        value.node_id != node_id ||
        value.literal_descriptor_handle != literal_descriptor_handle ||
        binding.literal_occurrence != literal_occurrence ||
        binding.node_id != node_id ||
        binding.literal_expression_id != literal_expression_id ||
        binding.comparison_expression_id != comparison_expression_id ||
        binding.target_expression_id != target_expression_id ||
        binding.source_node_id != source_node_id ||
        binding.literal_descriptor_handle != literal_descriptor_handle ||
        binding.target_descriptor_handle != target_descriptor_handle ||
        entry.comparison_occurrence != comparison_expression_id ||
        entry.target_descriptor_handle != target_descriptor_handle ||
        entry.literal_argument_ordinal != literal_argument_ordinal ||
        entry.target_argument_ordinal != target_argument_ordinal ||
        entry.equality_operation_uuid != equality_operation_uuid ||
        entry.equality_operation_generation !=
            equality_operation_generation) {
      continue;
    }
    if (found != nullptr) return nullptr;
    found = &entry;
  }
  return found;
}

std::span<const ContextualTextExecutionAuthorityEntryV2>
ViewContextualTextExecutionAuthorityLeaseV2(
    const ContextualTextExecutionAuthorityLeaseV2& lease) noexcept {
  if (!lease.valid()) return {};
  return lease.state_->entries;
}

const ContextualTextExecutionAuthorityEntryV2*
FindContextualTextExecutionAuthorityEntryV2(
    const ContextualTextExecutionAuthorityLeaseV2& lease,
    const std::uint32_t literal_expression_id,
    const std::uint32_t comparison_expression_id,
    const std::uint32_t target_expression_id,
    const std::uint32_t source_node_id,
    const std::uint32_t literal_descriptor_handle,
    const std::uint32_t target_descriptor_handle,
    const sblr::ContextualTextUuidV2& equality_operation_uuid,
    const std::uint64_t equality_operation_generation) noexcept {
  if (!lease.valid() || literal_expression_id == 0 ||
      comparison_expression_id == 0 || target_expression_id == 0 ||
      source_node_id == 0 || literal_descriptor_handle == 0 ||
      target_descriptor_handle == 0 || !Nonzero(equality_operation_uuid) ||
      equality_operation_generation == 0) {
    return nullptr;
  }
  const ContextualTextExecutionAuthorityEntryV2* found = nullptr;
  for (const auto& entry : lease.state_->entries) {
    const auto& binding = entry.runtime_materialization.graph_binding;
    if (binding.literal_expression_id != literal_expression_id ||
        binding.comparison_expression_id != comparison_expression_id ||
        binding.target_expression_id != target_expression_id ||
        binding.source_node_id != source_node_id ||
        binding.literal_descriptor_handle != literal_descriptor_handle ||
        binding.target_descriptor_handle != target_descriptor_handle ||
        entry.comparison_occurrence != comparison_expression_id ||
        entry.target_descriptor_handle != target_descriptor_handle ||
        entry.equality_operation_uuid != equality_operation_uuid ||
        entry.equality_operation_generation !=
            equality_operation_generation) {
      continue;
    }
    if (found != nullptr) return nullptr;
    found = &entry;
  }
  return found;
}

EngineApiDiagnostic RevokeContextualTextLiteralAuthorityV2(
    EngineContextualTextLiteralAuthorityHandleV2* handle,
    std::string_view reason) {
  if (handle == nullptr || !handle->valid())
    return Diagnostic("SBLR.CONTEXTUAL_TEXT_LITERAL.BINDING_STALE",
                      "engine.contextual_text_literal.revoke_invalid");
  const auto authority = handle->authority_;
  std::string receipt_key;
  {
    std::lock_guard<std::mutex> guard(authority->mutex);
    receipt_key = Hex(authority->request.statement_receipt_uuid);
    if (authority->state ==
        EngineContextualTextLiteralAuthorityStateV2::issued) {
      authority->state = EngineContextualTextLiteralAuthorityStateV2::revoked;
    }
    // A consumed execution/cursor lease owns its retained resources. Receipt
    // release drops only this receipt handle and cannot invalidate that lease.
  }
  {
    std::lock_guard<std::mutex> guard(g_authority_registry_mutex);
    const auto existing = g_authorities_by_receipt.find(receipt_key);
    if (existing != g_authorities_by_receipt.end()) {
      const auto retained = existing->second.lock();
      if (!retained || retained == authority)
        g_authorities_by_receipt.erase(existing);
    }
  }
  handle->authority_.reset();
  (void)reason;
  return OkDiagnostic();
}

}  // namespace scratchbird::engine::internal_api
