// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_metapage.hpp"

#include "index_route_capability.hpp"

#include <cstring>
#include <string_view>
#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::HostToLittle32;
using scratchbird::core::platform::HostToLittle64;
using scratchbird::core::platform::LittleToHost32;
using scratchbird::core::platform::LittleToHost64;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status ErrorStatus() { return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine}; }

inline constexpr u32 kCurrentMetadataFormatVersion = 2;
inline constexpr u32 kMinimumReaderFormatVersion = 1;
inline constexpr char kMetadataMagic[4] = {'M', 'D', '5', '2'};
inline constexpr u64 kHashSeedLow = 1469598103934665603ull;
inline constexpr u64 kHashSeedHigh = 1099511628211ull ^ 0x9e3779b97f4a7c15ull;

void Store32(std::vector<byte>* out, u32 value) {
  value = HostToLittle32(value);
  const auto* ptr = reinterpret_cast<const byte*>(&value);
  out->insert(out->end(), ptr, ptr + sizeof(value));
}
void Store64(std::vector<byte>* out, u64 value) {
  value = HostToLittle64(value);
  const auto* ptr = reinterpret_cast<const byte*>(&value);
  out->insert(out->end(), ptr, ptr + sizeof(value));
}
void StoreUuid(std::vector<byte>* out, const TypedUuid& uuid) {
  out->push_back(static_cast<byte>(uuid.kind));
  out->insert(out->end(), uuid.value.bytes.begin(), uuid.value.bytes.end());
}
u32 Load32(const std::vector<byte>& in, std::size_t* offset) {
  u32 value = 0;
  std::memcpy(&value, in.data() + *offset, sizeof(value));
  *offset += sizeof(value);
  return LittleToHost32(value);
}
u64 Load64(const std::vector<byte>& in, std::size_t* offset) {
  u64 value = 0;
  std::memcpy(&value, in.data() + *offset, sizeof(value));
  *offset += sizeof(value);
  return LittleToHost64(value);
}
TypedUuid LoadUuid(const std::vector<byte>& in, std::size_t* offset) {
  TypedUuid uuid;
  uuid.kind = static_cast<scratchbird::core::platform::UuidKind>(in[*offset]);
  *offset += 1;
  std::memcpy(uuid.value.bytes.data(), in.data() + *offset, uuid.value.bytes.size());
  *offset += uuid.value.bytes.size();
  return uuid;
}

u64 HashByte(u64 hash, byte value) {
  hash ^= static_cast<u64>(value);
  hash *= 1099511628211ull;
  return hash;
}

u64 HashU64(u64 hash, u64 value) {
  for (unsigned i = 0; i < 8; ++i) {
    hash = HashByte(hash, static_cast<byte>((value >> (i * 8)) & 0xffu));
  }
  return hash;
}

u64 HashU32(u64 hash, u32 value) {
  return HashU64(hash, value);
}

u64 HashBool(u64 hash, bool value) {
  return HashByte(hash, value ? static_cast<byte>(1) : static_cast<byte>(0));
}

u64 HashString(u64 hash, std::string_view value) {
  hash = HashU64(hash, static_cast<u64>(value.size()));
  for (const char ch : value) {
    hash = HashByte(hash, static_cast<byte>(ch));
  }
  return hash;
}

u64 HashTypedUuid(u64 hash, const TypedUuid& uuid) {
  hash = HashU32(hash, static_cast<u32>(uuid.kind));
  for (const byte value : uuid.value.bytes) {
    hash = HashByte(hash, value);
  }
  return hash;
}

u64 DescriptorHash(IndexFamily family) {
  const auto* descriptor = FindBuiltinIndexFamily(family);
  if (descriptor == nullptr) {
    return 0;
  }
  u64 hash = HashString(kHashSeedLow, "index_descriptor");
  hash = HashU32(hash, static_cast<u32>(descriptor->family));
  hash = HashString(hash, descriptor->id);
  hash = HashString(hash, descriptor->canonical_name);
  hash = HashTypedUuid(hash, descriptor->family_uuid);
  hash = HashU32(hash, static_cast<u32>(descriptor->persistence));
  hash = HashU32(hash, static_cast<u32>(descriptor->key_model));
  hash = HashU32(hash, static_cast<u32>(descriptor->completion));
  hash = HashString(hash, descriptor->native_physical_family);
  hash = HashString(hash, descriptor->default_semantic_profile);
  hash = HashBool(hash, descriptor->baseline);
  hash = HashBool(hash, descriptor->persistent);
  hash = HashBool(hash, descriptor->requires_mga_recheck);
  hash = HashBool(hash, descriptor->supports_ordering);
  hash = HashBool(hash, descriptor->supports_uniqueness);
  hash = HashBool(hash, descriptor->approximate);
  return hash;
}

u64 RouteCapabilityHash(IndexFamily family) {
  u64 hash = HashString(kHashSeedLow, "index_route_capability");
  u32 route_count = 0;
  for (const auto& route : BuiltinIndexRouteCapabilityStates()) {
    if (route.family != family) {
      continue;
    }
    ++route_count;
    hash = HashU32(hash, static_cast<u32>(route.route));
    hash = HashBool(hash, route.family_physical_complete);
    hash = HashBool(hash, route.route_declared);
    hash = HashBool(hash, route.route_supported);
    hash = HashBool(hash, route.supports_read);
    hash = HashBool(hash, route.supports_write);
    hash = HashBool(hash, route.supports_mutation);
    hash = HashBool(hash, route.supports_bulk_build);
    hash = HashBool(hash, route.supports_reopen);
    hash = HashBool(hash, route.supports_ordered_range);
    hash = HashBool(hash, route.supports_equality_lookup);
    hash = HashBool(hash, route.produces_candidate_set);
    hash = HashBool(hash, route.approximate_candidate_source);
    hash = HashBool(hash, route.requires_exact_recheck);
    hash = HashBool(hash, route.requires_mga_recheck);
    hash = HashBool(hash, route.requires_security_recheck);
    hash = HashBool(hash, route.benchmark_clean);
  }
  return route_count == 0 ? 0 : HashU32(hash, route_count);
}

u64 ProviderEvidenceHash(IndexFamily family) {
  const auto* state = FindBuiltinIndexFamilyPhysicalCapabilityState(family);
  if (state == nullptr) {
    return 0;
  }
  u64 hash = HashString(kHashSeedLow, "index_provider_evidence");
  hash = HashU32(hash, static_cast<u32>(state->family));
  hash = HashBool(hash, state->static_contract);
  hash = HashBool(hash, state->provider_present);
  hash = HashBool(hash, state->evidence_required);
  hash = HashBool(hash, state->provider_admitted);
  hash = HashBool(hash, state->durable_closure_admitted);
  hash = HashBool(hash, state->runtime_available);
  hash = HashBool(hash, state->benchmark_clean);
  hash = HashU32(hash, static_cast<u32>(state->blocker));
  hash = HashString(hash, state->blocker_diagnostic_code);
  hash = HashString(hash, state->blocker_message_key);
  return hash;
}

bool FamilyValidatorRequired(IndexFamily family) {
  const auto* descriptor = FindBuiltinIndexFamily(family);
  return descriptor != nullptr &&
         descriptor->persistence == IndexPersistenceClass::persistent &&
         descriptor->completion ==
             IndexCompletionStatus::accepted_requires_full_implementation;
}

bool FamilyValidatorPassed(IndexFamily family) {
  if (!FamilyValidatorRequired(family)) {
    return true;
  }
  const auto* state = FindBuiltinIndexFamilyPhysicalCapabilityState(family);
  return state != nullptr && state->validate && state->physically_complete();
}

u64 IdentityHash(const IndexMetapageControl& control) {
  u64 hash = HashString(kHashSeedLow, "index_identity");
  hash = HashTypedUuid(hash, control.index_uuid);
  hash = HashU32(hash, static_cast<u32>(control.family));
  hash = HashU64(hash, control.root_generation);
  hash = HashU64(hash, control.root_page_number);
  return hash;
}

std::pair<u64, u64> MetadataChecksum(const IndexMetapageControl& control) {
  u64 low = HashString(kHashSeedLow, "index_metapage_durable_metadata");
  u64 high = HashString(kHashSeedHigh, "index_metapage_durable_metadata");
  auto add = [&](u64 value) {
    low = HashU64(low, value);
    high = HashU64(high, value ^ 0xa0761d6478bd642full);
  };
  add(static_cast<u32>(control.family));
  add(static_cast<u32>(control.index_uuid.kind));
  for (const byte value : control.index_uuid.value.bytes) {
    low = HashByte(low, value);
    high = HashByte(high, static_cast<byte>(value ^ 0x5au));
  }
  add(control.root_page_number);
  add(control.resource_epoch);
  add(control.mutation_epoch);
  add(control.root_generation);
  add(control.page_count);
  add(control.tuple_count_estimate);
  add(control.layout_version);
  add(control.flags);
  low = HashString(low, control.semantic_profile_id);
  high = HashString(high, control.semantic_profile_id);
  add(control.metadata_format_version);
  add(control.minimum_reader_format_version);
  add(static_cast<u32>(control.checksum_profile));
  add(control.identity_hash);
  add(control.descriptor_hash);
  add(control.route_capability_hash);
  add(control.provider_evidence_hash);
  add(control.provider_evidence_count);
  add(control.family_validator_version);
  add(control.format_compatible ? 1 : 0);
  add(control.identity_bound ? 1 : 0);
  add(control.descriptor_hash_bound ? 1 : 0);
  add(control.route_capability_bound ? 1 : 0);
  add(control.provider_evidence_hash_bound ? 1 : 0);
  add(control.family_validator_required ? 1 : 0);
  add(control.family_validator_passed ? 1 : 0);
  if (control.checksum_profile == PageBodyChecksumProfile::fast) {
    high = 0;
  }
  return {low, high};
}

u32 DurableMetadataFlags(const IndexMetapageControl& control) {
  u32 flags = 0;
  if (control.format_compatible) flags |= 1u << 0;
  if (control.identity_bound) flags |= 1u << 1;
  if (control.descriptor_hash_bound) flags |= 1u << 2;
  if (control.route_capability_bound) flags |= 1u << 3;
  if (control.provider_evidence_hash_bound) flags |= 1u << 4;
  if (control.family_validator_required) flags |= 1u << 5;
  if (control.family_validator_passed) flags |= 1u << 6;
  return flags;
}

void ApplyDurableMetadataFlags(IndexMetapageControl* control, u32 flags) {
  control->format_compatible = (flags & (1u << 0)) != 0;
  control->identity_bound = (flags & (1u << 1)) != 0;
  control->descriptor_hash_bound = (flags & (1u << 2)) != 0;
  control->route_capability_bound = (flags & (1u << 3)) != 0;
  control->provider_evidence_hash_bound = (flags & (1u << 4)) != 0;
  control->family_validator_required = (flags & (1u << 5)) != 0;
  control->family_validator_passed = (flags & (1u << 6)) != 0;
}

void StoreDurableMetadata(std::vector<byte>* out,
                          const IndexMetapageControl& control) {
  out->insert(out->end(),
              kMetadataMagic,
              kMetadataMagic + sizeof(kMetadataMagic));
  Store32(out, control.metadata_format_version);
  Store32(out, control.minimum_reader_format_version);
  Store32(out, static_cast<u32>(control.checksum_profile));
  Store64(out, control.identity_hash);
  Store64(out, control.descriptor_hash);
  Store64(out, control.route_capability_hash);
  Store64(out, control.provider_evidence_hash);
  Store32(out, control.provider_evidence_count);
  Store32(out, control.family_validator_version);
  Store32(out, DurableMetadataFlags(control));
  Store64(out, control.metadata_checksum_low64);
  Store64(out, control.metadata_checksum_high64);
}

IndexMetapageResult MetadataError(const IndexMetapageControl& control,
                                  std::string detail) {
  IndexMetapageResult result;
  result.status = ErrorStatus();
  result.control = control;
  result.diagnostic = MakeIndexMetapageDiagnostic(
      result.status,
      "SB-INDEX-METAPAGE-DURABLE-METADATA-INVALID",
      "index.metapage.durable_metadata_invalid",
      std::move(detail));
  return result;
}
}  // namespace

IndexMetapageResult BuildIndexMetapageControl(const IndexMetapageControl& control) {
  IndexMetapageResult result;
  if (!control.index_uuid.valid() || control.family == IndexFamily::unknown) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexMetapageDiagnostic(result.status,
                                                    "SB-INDEX-METAPAGE-INVALID",
                                                    "index.metapage.invalid");
    return result;
  }
  const IndexMetapageControl durable =
      PopulateIndexMetapageDurableMetadata(control);
  const auto validation = ValidateIndexMetapageDurableMetadata(durable);
  if (!validation.ok()) {
    return validation;
  }
  result.status = OkStatus();
  result.control = durable;
  result.serialized = {'S', 'B', 'I', 'M'};
  StoreUuid(&result.serialized, durable.index_uuid);
  Store32(&result.serialized, static_cast<u32>(durable.family));
  Store64(&result.serialized, durable.root_page_number);
  Store64(&result.serialized, durable.resource_epoch);
  Store64(&result.serialized, durable.mutation_epoch);
  Store64(&result.serialized, durable.page_count);
  Store64(&result.serialized, durable.tuple_count_estimate);
  Store32(&result.serialized, durable.layout_version);
  Store32(&result.serialized, durable.flags);
  Store32(&result.serialized, static_cast<u32>(durable.semantic_profile_id.size()));
  result.serialized.insert(result.serialized.end(), durable.semantic_profile_id.begin(), durable.semantic_profile_id.end());
  Store64(&result.serialized, durable.root_generation);
  StoreDurableMetadata(&result.serialized, durable);
  return result;
}

IndexMetapageResult ParseIndexMetapageControl(const std::vector<byte>& serialized) {
  IndexMetapageResult result;
  if (serialized.size() < 4 + 17 + 4 + 8 * 5 + 4 + 4 + 4 ||
      serialized[0] != 'S' || serialized[1] != 'B' || serialized[2] != 'I' || serialized[3] != 'M') {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexMetapageDiagnostic(result.status,
                                                    "SB-INDEX-METAPAGE-BAD-ENVELOPE",
                                                    "index.metapage.bad_envelope");
    return result;
  }
  std::size_t offset = 4;
  result.control.index_uuid = LoadUuid(serialized, &offset);
  result.control.family = static_cast<IndexFamily>(Load32(serialized, &offset));
  result.control.root_page_number = Load64(serialized, &offset);
  result.control.resource_epoch = Load64(serialized, &offset);
  result.control.mutation_epoch = Load64(serialized, &offset);
  result.control.page_count = Load64(serialized, &offset);
  result.control.tuple_count_estimate = Load64(serialized, &offset);
  result.control.layout_version = Load32(serialized, &offset);
  result.control.flags = Load32(serialized, &offset);
  const u32 profile_size = Load32(serialized, &offset);
  if (serialized.size() < offset + profile_size) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexMetapageDiagnostic(result.status,
                                                    "SB-INDEX-METAPAGE-TRUNCATED",
                                                    "index.metapage.truncated");
    return result;
  }
  result.control.semantic_profile_id.assign(serialized.begin() + static_cast<std::ptrdiff_t>(offset),
                                            serialized.begin() + static_cast<std::ptrdiff_t>(offset + profile_size));
  offset += profile_size;
  if (serialized.size() >= offset + sizeof(u64)) {
    result.control.root_generation = Load64(serialized, &offset);
  } else if (serialized.size() != offset) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexMetapageDiagnostic(result.status,
                                                    "SB-INDEX-METAPAGE-TRUNCATED",
                                                    "index.metapage.truncated",
                                                    "root_generation");
    return result;
  }
  if (serialized.size() != offset) {
    if (serialized.size() < offset + sizeof(kMetadataMagic) ||
        std::memcmp(serialized.data() + offset,
                    kMetadataMagic,
                    sizeof(kMetadataMagic)) != 0) {
      result.status = ErrorStatus();
      result.diagnostic = MakeIndexMetapageDiagnostic(
          result.status,
          "SB-INDEX-METAPAGE-BAD-DURABLE-METADATA",
          "index.metapage.bad_durable_metadata",
          "metadata_extension_magic");
      return result;
    }
    offset += sizeof(kMetadataMagic);
    const std::size_t fixed_metadata_bytes =
        sizeof(u32) * 6u + sizeof(u64) * 6u;
    if (serialized.size() < offset + fixed_metadata_bytes) {
      result.status = ErrorStatus();
      result.diagnostic = MakeIndexMetapageDiagnostic(
          result.status,
          "SB-INDEX-METAPAGE-TRUNCATED",
          "index.metapage.truncated",
          "durable_metadata");
      return result;
    }
    result.control.durable_metadata_present = true;
    result.control.metadata_format_version = Load32(serialized, &offset);
    result.control.minimum_reader_format_version = Load32(serialized, &offset);
    result.control.checksum_profile =
        static_cast<PageBodyChecksumProfile>(Load32(serialized, &offset));
    result.control.identity_hash = Load64(serialized, &offset);
    result.control.descriptor_hash = Load64(serialized, &offset);
    result.control.route_capability_hash = Load64(serialized, &offset);
    result.control.provider_evidence_hash = Load64(serialized, &offset);
    result.control.provider_evidence_count = Load32(serialized, &offset);
    result.control.family_validator_version = Load32(serialized, &offset);
    ApplyDurableMetadataFlags(&result.control, Load32(serialized, &offset));
    result.control.metadata_checksum_low64 = Load64(serialized, &offset);
    result.control.metadata_checksum_high64 = Load64(serialized, &offset);
    if (serialized.size() != offset) {
      result.status = ErrorStatus();
      result.diagnostic = MakeIndexMetapageDiagnostic(
          result.status,
          "SB-INDEX-METAPAGE-BAD-DURABLE-METADATA",
          "index.metapage.bad_durable_metadata",
          "trailing_bytes");
      return result;
    }
    const auto validation =
        ValidateIndexMetapageDurableMetadata(result.control);
    if (!validation.ok()) {
      result.status = validation.status;
      result.diagnostic = validation.diagnostic;
      return result;
    }
  }
  result.status = OkStatus();
  result.serialized = serialized;
  return result;
}

IndexMetapageControl PopulateIndexMetapageDurableMetadata(
    const IndexMetapageControl& control) {
  IndexMetapageControl durable = control;
  durable.durable_metadata_present = true;
  if (durable.metadata_format_version == 0) {
    durable.metadata_format_version = kCurrentMetadataFormatVersion;
  }
  if (durable.minimum_reader_format_version == 0) {
    durable.minimum_reader_format_version = kMinimumReaderFormatVersion;
  }
  if (durable.checksum_profile == PageBodyChecksumProfile::unknown) {
    durable.checksum_profile = PageBodyChecksumProfile::strong;
  }
  durable.identity_hash = IdentityHash(durable);
  durable.descriptor_hash = DescriptorHash(durable.family);
  durable.route_capability_hash = RouteCapabilityHash(durable.family);
  durable.provider_evidence_hash = ProviderEvidenceHash(durable.family);
  durable.provider_evidence_count =
      FindBuiltinIndexFamilyPhysicalCapabilityState(durable.family) == nullptr
          ? 0
          : 1;
  durable.family_validator_version = durable.family_validator_version == 0
                                         ? 1
                                         : durable.family_validator_version;
  durable.format_compatible =
      durable.metadata_format_version == kCurrentMetadataFormatVersion &&
      durable.minimum_reader_format_version <= kCurrentMetadataFormatVersion &&
      durable.layout_version != 0 &&
      durable.checksum_profile != PageBodyChecksumProfile::unknown;
  durable.identity_bound = durable.index_uuid.valid() &&
                           durable.family != IndexFamily::unknown;
  durable.descriptor_hash_bound = durable.descriptor_hash != 0;
  durable.route_capability_bound = durable.route_capability_hash != 0;
  durable.provider_evidence_hash_bound =
      durable.provider_evidence_hash != 0 &&
      durable.provider_evidence_count != 0;
  durable.family_validator_required =
      FamilyValidatorRequired(durable.family);
  durable.family_validator_passed =
      FamilyValidatorPassed(durable.family);
  const auto checksum = MetadataChecksum(durable);
  durable.metadata_checksum_low64 = checksum.first;
  durable.metadata_checksum_high64 = checksum.second;
  return durable;
}

IndexMetapageResult ValidateIndexMetapageDurableMetadata(
    const IndexMetapageControl& control) {
  if (!control.durable_metadata_present) {
    return MetadataError(control, "durable_metadata_missing");
  }
  if (!control.index_uuid.valid() || control.family == IndexFamily::unknown) {
    return MetadataError(control, "identity_missing");
  }
  if (!control.format_compatible ||
      control.metadata_format_version != kCurrentMetadataFormatVersion ||
      control.minimum_reader_format_version > kCurrentMetadataFormatVersion ||
      control.layout_version == 0 ||
      control.checksum_profile == PageBodyChecksumProfile::unknown) {
    return MetadataError(control, "format_compatibility");
  }

  const IndexMetapageControl expected =
      PopulateIndexMetapageDurableMetadata(control);
  if (!control.identity_bound ||
      control.identity_hash != expected.identity_hash) {
    return MetadataError(control, "identity_hash");
  }
  if (!control.descriptor_hash_bound ||
      control.descriptor_hash != expected.descriptor_hash) {
    return MetadataError(control, "descriptor_hash");
  }
  if (!control.route_capability_bound ||
      control.route_capability_hash != expected.route_capability_hash) {
    return MetadataError(control, "route_capability_hash");
  }
  if (!control.provider_evidence_hash_bound ||
      control.provider_evidence_hash != expected.provider_evidence_hash ||
      control.provider_evidence_count != expected.provider_evidence_count) {
    return MetadataError(control, "provider_evidence_hash");
  }
  if (control.family_validator_required != expected.family_validator_required ||
      control.family_validator_passed != expected.family_validator_passed ||
      (control.family_validator_required &&
       !control.family_validator_passed)) {
    return MetadataError(control, "family_validator");
  }
  if (control.metadata_checksum_low64 != expected.metadata_checksum_low64 ||
      control.metadata_checksum_high64 != expected.metadata_checksum_high64) {
    return MetadataError(control, "metadata_checksum");
  }

  IndexMetapageResult result;
  result.status = OkStatus();
  result.control = control;
  return result;
}

DiagnosticRecord MakeIndexMetapageDiagnostic(Status status,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code, status.severity, status.subsystem,
                        std::move(diagnostic_code), std::move(message_key),
                        std::move(arguments), {}, "core.index.metapage");
}

}  // namespace scratchbird::core::index
