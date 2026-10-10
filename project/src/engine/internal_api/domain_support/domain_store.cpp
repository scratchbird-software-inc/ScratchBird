// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "domain_support/domain_store.hpp"
#include "domain_support/domain_base_descriptor_codec.hpp"
#include "query/expression_api.hpp"
#include "datatype_catalog_manifest.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "mga_relation_store/mga_binary_identity_codec.hpp"
#include "mga_relation_store/stored_scalar_payload.hpp"
#include "sbl_numeric.hpp"

#include "crud_support/crud_store.hpp"
#include "crud_support/native_value_payload.hpp"
#include "crud_support/retained_row_value_codec.hpp"
#include "datatype_operations.hpp"
#include "disk_device.hpp"
#include "dml/constraint_enforcement.hpp"
#include "hash_digest.hpp"
#include "runtime_platform.hpp"
#include "security/security_model.hpp"
#include "transaction/transaction_api.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace scratchbird::engine::internal_api {
namespace {
namespace dt = scratchbird::core::datatypes;
namespace core_hash = scratchbird::core::hash;
namespace disk = scratchbird::storage::disk;

using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

// SEARCH_KEY: SB_ENGINE_DOMAIN_BINARY_CATALOG_ENVELOPE
inline constexpr std::array<byte, 8> kDomainCatalogMagic = {
    'S', 'B', 'D', 'O', 'M', 'C', '0', '2'};
inline constexpr std::array<byte, 8> kDomainCatalogRecordMagic = {
    'S', 'B', 'D', 'O', 'M', 'R', '0', '2'};
inline constexpr u16 kDomainCatalogVersion = 2;
inline constexpr u16 kDomainCatalogHeaderBytes = 80;
inline constexpr u16 kDomainCatalogRecordHeaderBytes = 88;
inline constexpr u16 kDomainCatalogDigestBytes = core_hash::kSha256DigestBytes;
inline constexpr u32 kDomainStringFieldCount = 21;
inline constexpr u32 kDomainRecordFlagNullable = 1u << 0;
inline constexpr u32 kDomainRecordFlagDropped = 1u << 1;

inline constexpr u32 kCatalogOffsetVersion = 8;
inline constexpr u32 kCatalogOffsetHeaderBytes = 10;
inline constexpr u32 kCatalogOffsetFlags = 12;
inline constexpr u32 kCatalogOffsetGeneration = 16;
inline constexpr u32 kCatalogOffsetRecordCount = 24;
inline constexpr u32 kCatalogOffsetRecordBytes = 32;
inline constexpr u32 kCatalogOffsetDigestBytes = 40;
inline constexpr u32 kCatalogOffsetDigest = 44;

inline constexpr u32 kRecordOffsetVersion = 8;
inline constexpr u32 kRecordOffsetHeaderBytes = 10;
inline constexpr u32 kRecordOffsetAction = 12;
inline constexpr u32 kRecordOffsetFlags = 16;
inline constexpr u32 kRecordOffsetSequence = 24;
inline constexpr u32 kRecordOffsetCreatorTx = 32;
inline constexpr u32 kRecordOffsetPayloadBytes = 40;
inline constexpr u32 kRecordOffsetPayloadDigestBytes = 48;
inline constexpr u32 kRecordOffsetDigest = 52;

enum class DomainBinaryAction : u16 {
  create = 1,
  alter = 2,
  drop = 3,
};

struct BinaryDomainRecord {
  DomainBinaryAction action = DomainBinaryAction::create;
  u64 sequence = 0;
  DomainRecord record;
};

struct BinaryCatalogLoadResult {
  bool ok = false;
  bool present = false;
  EngineApiDiagnostic diagnostic;
  std::vector<BinaryDomainRecord> records;
};

std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream in(value);
  while (std::getline(in, current, delimiter)) { parts.push_back(current); }
  return parts;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return 10 + c - 'a'; }
  if (c >= 'A' && c <= 'F') { return 10 + c - 'A'; }
  return -1;
}

std::string HexDecode(const std::string& value) {
  std::string out;
  if ((value.size() % 2) != 0) { return out; }
  out.reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    const int hi = HexValue(value[i]);
    const int lo = HexValue(value[i + 1]);
    if (hi < 0 || lo < 0) { return {}; }
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

std::uint64_t ParseU64(const std::string& value) {
  try { return static_cast<std::uint64_t>(std::stoull(value)); } catch (...) { return 0; }
}

bool ParseBool(const std::string& value) { return value == "1" || value == "true" || value == "TRUE"; }

bool MagicEquals(const std::vector<byte>& bytes,
                 std::size_t offset,
                 const std::array<byte, 8>& magic) {
  if (bytes.size() < offset + magic.size()) { return false; }
  return std::equal(magic.begin(),
                    magic.end(),
                    bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

void Store16(std::vector<byte>* out, std::size_t offset, u16 value) {
  StoreLittle16(out->data() + offset, value);
}

void Store32(std::vector<byte>* out, std::size_t offset, u32 value) {
  StoreLittle32(out->data() + offset, value);
}

void Store64(std::vector<byte>* out, std::size_t offset, u64 value) {
  StoreLittle64(out->data() + offset, value);
}

u16 Load16(const std::vector<byte>& bytes, std::size_t offset) {
  return LoadLittle16(bytes.data() + offset);
}

u32 Load32(const std::vector<byte>& bytes, std::size_t offset) {
  return LoadLittle32(bytes.data() + offset);
}

u64 Load64(const std::vector<byte>& bytes, std::size_t offset) {
  return LoadLittle64(bytes.data() + offset);
}

void Append32(std::vector<byte>* out, u32 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  Store32(out, offset, value);
}

bool AppendLengthPrefixedString(std::vector<byte>* out, const std::string& value) {
  if (value.size() > std::numeric_limits<u32>::max()) { return false; }
  Append32(out, static_cast<u32>(value.size()));
  out->insert(out->end(),
              reinterpret_cast<const byte*>(value.data()),
              reinterpret_cast<const byte*>(value.data()) + value.size());
  return true;
}

bool ReadLengthPrefixedString(const std::vector<byte>& payload,
                              std::size_t* offset,
                              std::string* out) {
  if (*offset > payload.size() || payload.size() - *offset < sizeof(u32)) { return false; }
  const u32 length = Load32(payload, *offset);
  *offset += sizeof(u32);
  if (payload.size() - *offset < length) { return false; }
  out->assign(reinterpret_cast<const char*>(payload.data() + *offset), length);
  *offset += length;
  return true;
}

std::vector<std::string> DomainStringFields(const DomainRecord& record) {
  return {
      record.default_name,
      record.base_descriptor_kind,
      record.base_canonical_type_name,
      record.base_encoded_descriptor,
      record.default_expression_envelope,
      record.check_constraint_envelope,
      record.charset_or_collation_ref,
      record.numeric_metadata,
      record.validation_hook_status,
      record.cast_policy_envelope,
      record.mutation_policy_envelope,
      record.masking_policy_envelope,
      record.visibility_policy_envelope,
      record.encryption_policy_ref,
      record.driver_metadata_envelope,
      record.wire_metadata_envelope,
      record.element_path_envelope,
      record.method_binding_envelope,
      record.localized_names_envelope,
      record.comment_envelope,
      record.reference_alias_envelope,
  };
}

bool AssignDomainStringFields(const std::vector<std::string>& fields, DomainRecord* record) {
  if (fields.size() != kDomainStringFieldCount) { return false; }
  std::size_t index = 0;
  record->default_name = fields[index++];
  record->base_descriptor_kind = fields[index++];
  record->base_canonical_type_name = fields[index++];
  record->base_encoded_descriptor = fields[index++];
  record->default_expression_envelope = fields[index++];
  record->check_constraint_envelope = fields[index++];
  record->charset_or_collation_ref = fields[index++];
  record->numeric_metadata = fields[index++];
  record->validation_hook_status = fields[index++];
  record->cast_policy_envelope = fields[index++];
  record->mutation_policy_envelope = fields[index++];
  record->masking_policy_envelope = fields[index++];
  record->visibility_policy_envelope = fields[index++];
  record->encryption_policy_ref = fields[index++];
  record->driver_metadata_envelope = fields[index++];
  record->wire_metadata_envelope = fields[index++];
  record->element_path_envelope = fields[index++];
  record->method_binding_envelope = fields[index++];
  record->localized_names_envelope = fields[index++];
  record->comment_envelope = fields[index++];
  record->reference_alias_envelope = fields[index++];
  return true;
}

std::vector<byte> DigestInputWithZeroedRange(std::vector<byte> encoded,
                                             std::size_t offset,
                                             std::size_t bytes) {
  if (encoded.size() >= offset + bytes) {
    std::fill(encoded.begin() + static_cast<std::ptrdiff_t>(offset),
              encoded.begin() + static_cast<std::ptrdiff_t>(offset + bytes),
              byte{0});
  }
  return encoded;
}

bool Sha256DigestMatches(const std::vector<byte>& encoded,
                         std::size_t digest_offset,
                         std::size_t digest_bytes) {
  if (digest_bytes != kDomainCatalogDigestBytes ||
      encoded.size() < digest_offset + digest_bytes) {
    return false;
  }
  const auto digest_input =
      DigestInputWithZeroedRange(encoded, digest_offset, digest_bytes);
  const auto computed = core_hash::ComputeSha256Digest(digest_input);
  if (!computed.ok()) { return false; }
  const std::vector<byte> stored(
      encoded.begin() + static_cast<std::ptrdiff_t>(digest_offset),
      encoded.begin() + static_cast<std::ptrdiff_t>(digest_offset + digest_bytes));
  return core_hash::ConstantTimeEqual(stored, core_hash::DigestVector(computed.digest));
}

bool AttachSha256Digest(std::vector<byte>* encoded,
                        std::size_t digest_offset,
                        std::size_t digest_bytes) {
  if (digest_bytes != kDomainCatalogDigestBytes ||
      encoded->size() < digest_offset + digest_bytes) {
    return false;
  }
  std::fill(encoded->begin() + static_cast<std::ptrdiff_t>(digest_offset),
            encoded->begin() + static_cast<std::ptrdiff_t>(digest_offset + digest_bytes),
            byte{0});
  const auto computed = core_hash::ComputeSha256Digest(*encoded);
  if (!computed.ok()) { return false; }
  const auto digest = core_hash::DigestVector(computed.digest);
  std::copy(digest.begin(),
            digest.end(),
            encoded->begin() + static_cast<std::ptrdiff_t>(digest_offset));
  return true;
}

std::string DomainCatalogPath(const EngineRequestContext& context) {
  return context.database_path + ".sb.domain_catalog";
}

bool TxVisible(const CrudState& state, std::uint64_t creator_tx, std::uint64_t observer_tx) {
  const auto it = state.transactions.find(creator_tx);
  if (it == state.transactions.end()) { return false; }
  if (it->second == "committed") { return true; }
  return creator_tx == observer_tx && it->second == "active";
}

bool StartsWith(const std::string& value, const std::string& prefix) { return value.rfind(prefix, 0) == 0; }

std::string LowerAscii(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  return value;
}

bool HasDomainRight(const EngineRequestContext& context,
                    const EngineUuid& domain_uuid,
                    const std::string& right) {
  return context.security_context_present &&
         SecurityContextHasRight(context, right, domain_uuid);
}

std::string RequiredRightFromPolicy(const std::string& policy) {
  if (StartsWith(policy, "require_right:")) { return policy.substr(14); }
  return {};
}

std::string NormalizeDomainPath(std::string path) {
  while (!path.empty() && (path.front() == '/' || path.front() == '.' || path.front() == '$')) {
    path.erase(path.begin());
  }
  for (char& c : path) {
    if (c == '/') { c = '.'; }
  }
  return path;
}

CrudStoredValue FieldValue(const CrudValueFields& values, const std::string& field) {
  for (const auto& [name, value] : values) {
    if (name == field) { return value; }
  }
  return CrudStoredValue::Missing();
}

bool HasField(const CrudValueFields& values, const std::string& field) {
  for (const auto& [name, ignored] : values) {
    if (name == field) { return true; }
  }
  return false;
}

void UpsertField(CrudValueFields* values,
                 const std::string& field,
                 const CrudStoredValue& value) {
  for (auto& [name, existing] : *values) {
    if (name == field) {
      existing = value;
      return;
    }
  }
  values->push_back({field, value});
}

std::string DescriptorField(const std::string& descriptor, const std::string& key) {
  const std::string prefix = key + "=";
  for (const auto& part : Split(descriptor, ';')) {
    if (StartsWith(part, prefix)) { return part.substr(prefix.size()); }
  }
  return {};
}

EngineApiDiagnostic DomainValidationDiagnostic(const std::string& detail) {
  if (StartsWith(detail, "domain_check_")) {
    return MakeEngineApiDiagnostic("SBSQL_DOMAIN_CHECK_VIOLATION",
                                   "sbsql.domain.check_violation",
                                   "domain.validate_value:" + detail);
  }
  if (detail == "domain_null_forbidden" ||
      StartsWith(detail, "domain_required_column_missing:")) {
    return MakeEngineApiDiagnostic("SBSQL_CONSTRAINT_VIOLATION",
                                   "sbsql.constraint.violation",
                                   "domain.validate_value:" + detail);
  }
  if (detail.find("_denied") != std::string::npos) {
    return MakeEngineApiDiagnostic("SECURITY.AUTHORIZATION.DENIED",
                                   "security.authorization.denied",
                                   "domain.validate_value:" + detail);
  }
  return MakeEngineApiDiagnostic("SBSQL_DOMAIN_VALIDATION_FAILED",
                                 "sbsql.domain.validation_failed",
                                 "domain.validate_value:" + detail);
}

bool AdmitDomainScalarBaseDescriptor(const EngineRequestContext& context,
                                     const EngineDescriptor& descriptor,
                                     std::string* detail) {
  if (!DomainBaseDescriptorStructureValidV1(descriptor)) {
    *detail = "domain_base_descriptor_structure_invalid";
    return false;
  }
  const auto bound = dt::LookupDatatypeTypeCodecIdentityV1(
      context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
      context.datatype_registry_generation, descriptor.datatype_descriptor_uuid,
      descriptor.datatype_descriptor_generation);
  if (!bound.ok || bound.row.type_uuid != descriptor.type_uuid) {
    *detail = "domain_base_datatype_cohort_mismatch";
    return false;
  }
  scratchbird::engine::ExecutionTypeDescriptor execution;
  return QowResolveBoundExecutionDescriptorV1(descriptor, &execution, detail);
}

bool ResolveDomainScalarBaseDescriptor(const EngineRequestContext& context,
                                       const DomainRecord& domain,
                                       std::uint64_t observer_tx,
                                       EngineDescriptor* descriptor,
                                       std::string* detail,
                                       std::string* proof_material = nullptr,
                                       std::vector<DomainRecord>* resolved_chain = nullptr) {
  // The caller holds the inventory guard. Resolve one catalog image and one
  // visibility image, not a fresh full catalog read for every ancestor.
  const auto catalog = LoadDomainState(context);
  const auto crud = LoadCrudState(context);
  if (!catalog.ok || !crud.ok) { *detail = "domain_catalog_read_failed"; return false; }
  std::map<EngineUuid, const DomainRecord*> visible;
  for (const auto& record : catalog.domains) {
    if (!TxVisible(crud.state, record.creator_tx, observer_tx)) continue;
    if (record.dropped) visible.erase(record.domain_uuid);
    else visible[record.domain_uuid] = &record;
  }
  const auto root = visible.find(domain.domain_uuid);
  if (root == visible.end() || MakeDomainCreateEvent(*root->second) != MakeDomainCreateEvent(domain)) {
    *detail = "domain_root_binding_not_current";
    return false;
  }
  const DomainRecord* cursor = root->second;
  std::set<EngineUuid> visited;
  std::vector<const DomainRecord*> chain;
  std::string material;
  std::size_t chain_bytes = 0;
  while (true) {
    if (!visited.insert(cursor->domain_uuid).second) {
      *detail = "domain_chain_cycle_detected";
      return false;
    }
    const auto encoded = MakeDomainCreateEvent(*cursor);
    if (encoded.empty() || chain_bytes > kApiBehaviorRecordMaximumBytes - sizeof(std::uint32_t) ||
        encoded.size() > kApiBehaviorRecordMaximumBytes - sizeof(std::uint32_t) - chain_bytes) {
      *detail = "domain_chain_size_limit";
      return false;
    }
    chain_bytes += sizeof(std::uint32_t) + encoded.size();
    if (proof_material) AppendBinaryString(&material, encoded);
    chain.push_back(cursor);
    if (cursor->base_descriptor_kind != "domain") break;
    const auto next = visible.find(cursor->base_descriptor_uuid);
    if (next == visible.end()) {
      *detail = "domain_base_not_visible";
      return false;
    }
    cursor = next->second;
  }
  DomainInheritedBaseBindingV1 inherited;
  if (!DecodeDomainInheritedBaseBindingV1(cursor->base_encoded_descriptor, &inherited)) {
    *detail = "domain_base_profile_binding_invalid";
    return false;
  }
  auto staged = inherited.base;
  const auto registered = dt::LookupDatatypeTypeCodecIdentityV3(
      context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
      context.datatype_registry_generation, staged.datatype_descriptor_uuid, staged.datatype_descriptor_generation);
  if (!registered.ok ||
      inherited != CaptureDomainInheritedBaseBindingV1(staged, registered.row)) {
    *detail = "domain_inherited_profile_stale";
    return false;
  }
  for (const auto* ancestor : chain) {
    DomainInheritedBaseBindingV1 parent_binding;
    if (!DecodeDomainInheritedBaseBindingV1(ancestor->base_encoded_descriptor, &parent_binding) ||
        parent_binding != inherited || ancestor->base_canonical_type_name != staged.canonical_type_name) {
      *detail = "domain_inherited_ancestor_profile_stale";
      return false;
    }
  }
  if (
      cursor->base_descriptor_uuid != staged.datatype_descriptor_uuid ||
      cursor->base_descriptor_kind != staged.descriptor_kind ||
      cursor->base_canonical_type_name != staged.canonical_type_name) {
    *detail = "domain_base_descriptor_binding_invalid";
    return false;
  }
  if (!AdmitDomainScalarBaseDescriptor(context, staged, detail)) return false;
  if (resolved_chain) {
    std::vector<DomainRecord> records;
    records.reserve(chain.size());
    for (const auto* record : chain) records.push_back(*record);
    *resolved_chain = std::move(records);
  }
  if (proof_material) *proof_material = std::move(material);
  *descriptor = std::move(staged);
  return true;
}

bool BindDomainLiteralCharacterDescriptor(const EngineRequestContext& context,
                                           EngineDescriptor* descriptor,
                                           std::string* detail) {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) { *detail = "domain_literal_catalog_unavailable"; return false; }
  const auto selected = dt::LookupDatatypeCatalogRow(manifest.manifest, dt::CanonicalTypeId::character);
  if (!selected.ok() || selected.manifest.descriptor_rows.size() != 1) {
    *detail = "domain_literal_datatype_not_unique"; return false;
  }
  const auto& row = selected.manifest.descriptor_rows.front();
  const auto binding = dt::LookupDatatypeTypeCodecIdentityV1(
      context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
      context.datatype_registry_generation, row.descriptor_uuid.value, row.descriptor_epoch);
  if (!binding.ok) { *detail = "domain_literal_datatype_cohort_mismatch"; return false; }
  EngineDescriptor staged;
  staged.descriptor_uuid = GenerateCrudEngineUuid("object");
  staged.descriptor_kind = "scalar";
  staged.canonical_type_name = row.stable_name;
  staged.datatype_descriptor_uuid = row.descriptor_uuid.value;
  staged.datatype_descriptor_generation = row.descriptor_epoch;
  staged.type_uuid = binding.row.type_uuid;
  staged.encoded_descriptor = "nullable=false";
  if (!AdmitDomainScalarBaseDescriptor(context, staged, detail)) return false;
  *descriptor = std::move(staged);
  return true;
}

bool NumberCompare(const EngineDescriptor& descriptor, const std::string& left,
                    std::string_view op, std::string_view right) {
  namespace numeric = scratchbird::libraries::sbl_numeric;
  const auto type = dt::CanonicalTypeIdFromStableName(descriptor.canonical_type_name);
  const bool signed_integer = type == dt::CanonicalTypeId::int8 || type == dt::CanonicalTypeId::int16 ||
      type == dt::CanonicalTypeId::int32 || type == dt::CanonicalTypeId::int64 || type == dt::CanonicalTypeId::int128;
  const bool unsigned_integer = type == dt::CanonicalTypeId::uint8 || type == dt::CanonicalTypeId::uint16 ||
      type == dt::CanonicalTypeId::uint32 || type == dt::CanonicalTypeId::uint64 || type == dt::CanonicalTypeId::uint128;
  const auto matches = [&](int comparison) {
    if (op == "gt") return comparison > 0;
    if (op == "gte") return comparison >= 0;
    if (op == "lt") return comparison < 0;
    if (op == "lte") return comparison <= 0;
    return op == "eq" && comparison == 0;
  };
  if (signed_integer || unsigned_integer) {
    const auto compared = numeric::CompareIntegerLittleEndianToDecimalLiteral(
        reinterpret_cast<const std::uint8_t*>(left.data()), left.size(), signed_integer, right);
    return compared.status == numeric::NumericStatusCode::ok && matches(compared.comparison);
  }
  dt::DatatypeComparisonRequest request;
  request.left = {dt::CanonicalTypeId::decimal, left, false};
  request.right = {dt::CanonicalTypeId::decimal, std::string(right), false};
  const auto compared = dt::CompareDatatypeValues(request);
  if (!compared.ok()) { return false; }
  if (op == "gt") { return compared.comparison > 0; }
  if (op == "gte") { return compared.comparison >= 0; }
  if (op == "lt") { return compared.comparison < 0; }
  if (op == "lte") { return compared.comparison <= 0; }
  if (op == "eq") { return compared.comparison == 0; }
  return false;
}

bool ParseFullNumberLiteral(std::string_view value) {
  return scratchbird::libraries::sbl_numeric::ValidateExactDecimalLiteral(value);
}

bool ParseFullLengthLiteral(std::string_view value, std::size_t* out) {
  if (value.empty()) { return false; }
  try {
    std::size_t parsed = 0;
    const auto length = static_cast<std::size_t>(std::stoull(std::string(value), &parsed));
    if (parsed != value.size()) { return false; }
    if (out != nullptr) { *out = length; }
    return true;
  } catch (...) {
    return false;
  }
}

bool IsNumericDomainCheckOp(std::string_view op) {
  return op == "gt" || op == "gte" || op == "lt" || op == "lte" || op == "eq";
}

bool IsLengthDomainCheckOp(std::string_view op) {
  return op == "length_gt" || op == "length_gte" || op == "length_lt" || op == "length_lte";
}

template<class Visitor>
bool VisitDomainCheckTerms(std::string_view predicate, Visitor visitor) {
  if (predicate.empty() || predicate.size() > kApiBehaviorRecordMaximumBytes) return false;
  const bool conjunction = predicate.starts_with("all:");
  const auto strip_all = [](std::string_view term) {
    while (term.starts_with("all:")) term.remove_prefix(4);
    return term;
  };
  predicate = strip_all(predicate);
  if (!conjunction) return !predicate.empty() && visitor(predicate);
  for (;;) {
    const auto separator = predicate.find(';');
    const auto term = strip_all(predicate.substr(0, separator));
    if (term.empty() || !visitor(term)) return false;
    if (separator == std::string_view::npos) return true;
    predicate.remove_prefix(separator + 1);
  }
}

bool IsSupportedDomainCheckLeaf(std::string_view predicate) {
  if (predicate == "not_empty") { return true; }
  const auto pos = predicate.find(':');
  if (pos == std::string::npos) { return false; }
  const auto op = predicate.substr(0, pos);
  const auto rhs = predicate.substr(pos + 1);
  if (IsNumericDomainCheckOp(op)) { return ParseFullNumberLiteral(rhs); }
  if (IsLengthDomainCheckOp(op)) { return ParseFullLengthLiteral(rhs, nullptr); }
  return false;
}

bool IsSupportedDomainCheckPredicate(std::string_view predicate) {
  return VisitDomainCheckTerms(predicate, IsSupportedDomainCheckLeaf);
}

bool EvaluateDomainCheckLeaf(std::string_view predicate,
                                  const EngineDescriptor& descriptor,
                                  const std::string& value,
                                  std::string* rejection_detail) {
  if (predicate == "not_empty") {
    if (!value.empty()) { return true; }
    *rejection_detail = "domain_check_not_empty_failed";
    return false;
  }
  const auto pos = predicate.find(':');
  if (pos == std::string::npos) {
    *rejection_detail = "domain_check_constraint_requires_executor_expression_support";
    return false;
  }
  const auto op = predicate.substr(0, pos);
  const auto rhs = predicate.substr(pos + 1);
  if (IsNumericDomainCheckOp(op)) {
    if (!ParseFullNumberLiteral(rhs)) {
      *rejection_detail = "domain_check_numeric_rhs_invalid";
      return false;
    }
    if (NumberCompare(descriptor, value, op, rhs)) { return true; }
    *rejection_detail = "domain_check_" + std::string(op) + "_failed";
    return false;
  }
  if (IsLengthDomainCheckOp(op)) {
    const std::size_t length = value.size();
    std::size_t rhs_length = 0;
    if (!ParseFullLengthLiteral(rhs, &rhs_length)) {
      *rejection_detail = "domain_check_length_rhs_invalid";
      return false;
    }
    if ((op == "length_gt" && length > rhs_length) || (op == "length_gte" && length >= rhs_length) ||
        (op == "length_lt" && length < rhs_length) || (op == "length_lte" && length <= rhs_length)) {
      return true;
    }
    *rejection_detail = "domain_check_" + std::string(op) + "_failed";
    return false;
  }
  *rejection_detail = "domain_check_constraint_requires_executor_expression_support";
  return false;
}

bool EvaluateDomainCheckPredicate(std::string_view predicate,
                                  const EngineDescriptor& descriptor,
                                  const std::string& value,
                                  std::string* rejection_detail) {
  const bool ok = VisitDomainCheckTerms(predicate, [&](std::string_view term) {
    return EvaluateDomainCheckLeaf(term, descriptor, value, rejection_detail);
  });
  if (!ok && rejection_detail->empty())
    *rejection_detail = "domain_check_constraint_requires_executor_expression_support";
  return ok;
}

bool CheckConstraintPasses(const std::string& envelope, const EngineDescriptor& descriptor,
                           const std::string& value, std::string* rejection_detail) {
  if (envelope.empty()) { return true; }
  if (StartsWith(envelope, "sblr_predicate:")) {
    const std::string predicate = envelope.substr(15);
    if (!IsSupportedDomainCheckPredicate(predicate)) {
      *rejection_detail = "domain_sblr_predicate_unsupported";
      return false;
    }
    std::string inner_detail;
    if (EvaluateDomainCheckPredicate(predicate, descriptor, value, &inner_detail)) { return true; }
    if (inner_detail == "domain_check_not_empty_failed") {
      *rejection_detail = "domain_sblr_predicate_not_empty_failed";
    } else {
      *rejection_detail = "domain_sblr_predicate_" + inner_detail;
    }
    return false;
  }
  if (!IsSupportedDomainCheckPredicate(envelope)) {
    *rejection_detail = "domain_check_constraint_requires_executor_expression_support";
    return false;
  }
  return EvaluateDomainCheckPredicate(envelope, descriptor, value, rejection_detail);
}

bool DomainVisibilityAllowsRead(const EngineRequestContext& context,
                                const DomainRecord& domain,
                                const std::string& column_name,
                                std::string* rejection_detail) {
  const std::string raw_policy = domain.visibility_policy_envelope;
  const std::string policy = LowerAscii(raw_policy);
  if (policy.empty() || policy == "allow_all") { return true; }
  if (policy == "deny_all") {
    *rejection_detail = "domain_visibility_denied:" + column_name;
    return false;
  }
  if (policy == "require_security_context") {
    if (context.security_context_present) { return true; }
    *rejection_detail = "domain_visibility_requires_security_context:" + column_name;
    return false;
  }
  if (StartsWith(policy, "require_principal:")) {
    const auto required=std::string_view(domain.visibility_policy_envelope).substr(18);
    if(required.size()==16){EngineUuid identity;std::copy_n(reinterpret_cast<const std::uint8_t*>(required.data()),16,identity.bytes.begin());if(core::uuid::IsEngineIdentityUuid(identity)&&context.principal_uuid==identity)return true;}
    *rejection_detail = "domain_visibility_principal_denied:" + column_name;
    return false;
  }
  const std::string required_right = StartsWith(policy, "require_right:") ? raw_policy.substr(14) : std::string{};
  if (!required_right.empty()) {
    if (HasDomainRight(context, domain.domain_uuid, required_right)) { return true; }
    *rejection_detail = "domain_visibility_right_denied:" + column_name + ":" + required_right;
    return false;
  }
  *rejection_detail = "domain_visibility_policy_unsupported:" + column_name;
  return false;
}

bool DomainEncryptionAllowsRead(const EngineRequestContext& context,
                                const DomainRecord& domain,
                                const std::string& column_name,
                                std::string* rejection_detail) {
  if (domain.encryption_policy_ref.empty()) { return true; }
  if (StartsWith(domain.encryption_policy_ref, "key_policy:")) {
    const std::string key = domain.encryption_policy_ref.substr(11);
    if (!key.empty() && (HasDomainRight(context, domain.domain_uuid, "DOMAIN_KEY_USE:" + key) ||
                         HasDomainRight(context, domain.domain_uuid, "DOMAIN_KEY_ADMIN:" + key))) {
      return true;
    }
    *rejection_detail = "domain_encryption_key_policy_denied:" + column_name;
    return false;
  }
  if (context.security_context_present) { return true; }
  *rejection_detail = "domain_encryption_policy_requires_security_context:" + column_name;
  return false;
}

std::string ApplyPrimitiveMask(const std::string& policy, const std::string& value) {
  if (policy.empty() || policy == "none") { return value; }
  if (policy == "mask_all") { return "****"; }
  if (policy == "null") { return "<NULL>"; }
  if (StartsWith(policy, "fixed:")) { return policy.substr(6); }
  if (policy == "last4") {
    if (value.size() <= 4) { return "****"; }
    return std::string(value.size() - 4, '*') + value.substr(value.size() - 4);
  }
  return "****";
}

struct PathMaskResult {
  bool ok = false;
  CrudStoredValue value;
  bool masked = false;
  bool unmasked = false;
  std::string rejection_detail;
};

std::vector<std::pair<std::string, std::string>> ParsePathValue(const std::string& value) {
  std::vector<std::pair<std::string, std::string>> fields;
  for (const auto& part : Split(value, ';')) {
    const auto pos = part.find('=');
    if (pos == std::string::npos || pos == 0) { return {}; }
    fields.push_back({NormalizeDomainPath(part.substr(0, pos)), part.substr(pos + 1)});
  }
  return fields;
}

std::string SerializePathValue(const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string out;
  for (const auto& [path, value] : fields) {
    if (!out.empty()) { out.push_back(';'); }
    out.append(path);
    out.push_back('=');
    out.append(value);
  }
  return out;
}

PathMaskResult ApplyDomainMaskPolicy(const EngineRequestContext& context,
                                     const DomainRecord& domain,
                                     const std::string& column_name,
                                     const std::string& value) {
  PathMaskResult result;
  result.value = value;
  const std::string policy = domain.masking_policy_envelope;
  if (policy.empty() || policy == "none") {
    result.ok = true;
    return result;
  }
  if (HasDomainRight(context, domain.domain_uuid, "DOMAIN_UNMASK")) {
    result.ok = true;
    result.unmasked = true;
    return result;
  }
  if (!StartsWith(policy, "path:") && policy.find("|path:") == std::string::npos) {
    result.value = policy == "null" ? CrudStoredValue::SqlNull()
                                    : CrudStoredValue(ApplyPrimitiveMask(policy, value));
    result.masked = result.value != value;
    result.ok = true;
    return result;
  }

  auto fields = ParsePathValue(value);
  if (fields.empty()) {
    result.rejection_detail = "domain_path_value_invalid:" + column_name;
    return result;
  }
  for (const auto& raw_rule : Split(policy, '|')) {
    if (!StartsWith(raw_rule, "path:")) { continue; }
    const auto rule = raw_rule.substr(5);
    const auto pos = rule.find(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= rule.size()) {
      result.rejection_detail = "domain_path_mask_rule_invalid:" + column_name;
      return result;
    }
    const std::string path = NormalizeDomainPath(rule.substr(0, pos));
    const std::string mask = rule.substr(pos + 1);
    bool found = false;
    for (auto& [field_path, field_value] : fields) {
      if (field_path == path) {
        field_value = ApplyPrimitiveMask(mask, field_value);
        found = true;
        result.masked = true;
      }
    }
    if (!found) {
      result.rejection_detail = "domain_path_missing:" + column_name + ":" + path;
      return result;
    }
  }
  result.value = SerializePathValue(fields);
  result.ok = true;
  return result;
}

std::optional<CrudStoredValue> MaterializeDefault(const std::string& envelope) {
  if (StartsWith(envelope, "literal:")) { return envelope.substr(8); }
  if (StartsWith(envelope, "value:")) { return envelope.substr(6); }
  return std::nullopt;
}

bool EncodeBinaryDomainRecord(const BinaryDomainRecord&,std::vector<byte>*);
bool DecodeBinaryDomainRecord(const std::vector<byte>&,std::size_t*,u64*,BinaryDomainRecord*,std::string*);
std::string MakeDomainEvent(const char* action,const DomainRecord& record){
  BinaryDomainRecord value;value.action=std::string_view(action)=="DOMAIN_CREATE"?DomainBinaryAction::create:std::string_view(action)=="DOMAIN_ALTER"?DomainBinaryAction::alter:DomainBinaryAction::drop;
  value.sequence=1;value.record=record;std::vector<byte> encoded;
  if(!EncodeBinaryDomainRecord(value,&encoded))return {};
  return {reinterpret_cast<const char*>(encoded.data()),encoded.size()};
}
bool DecodeDomainEvent(const std::string& event,DomainBinaryAction* action,DomainRecord* record){
  const std::vector<byte> bytes(event.begin(),event.end());std::size_t offset=0;u64 sequence=0;BinaryDomainRecord value;std::string detail;
  if(!DecodeBinaryDomainRecord(bytes,&offset,&sequence,&value,&detail)||offset!=bytes.size())return false;
  *action=value.action;*record=std::move(value.record);return true;
}

std::string DomainEventPath(const EngineRequestContext& context) {
  return context.database_path + ".sb.domain_events";
}

EngineApiDiagnostic DomainCatalogDiagnostic(const std::string& operation_id,
                                            const std::string& detail) {
  return MakeInvalidRequestDiagnostic(operation_id, detail);
}

BinaryCatalogLoadResult BinaryCatalogAbsent() {
  BinaryCatalogLoadResult result;
  result.ok = true;
  result.present = false;
  return result;
}

BinaryCatalogLoadResult BinaryCatalogError(const std::string& operation_id,
                                           const std::string& detail) {
  BinaryCatalogLoadResult result;
  result.ok = false;
  result.present = true;
  result.diagnostic = DomainCatalogDiagnostic(operation_id, detail);
  return result;
}

bool SerializeDomainPayload(const DomainRecord& record, std::vector<byte>* payload) {
  if(!core::uuid::IsEngineIdentityUuid(record.domain_uuid))return false;
  for(const auto& id:{record.domain_uuid,record.catalog_row_uuid,record.schema_uuid,record.base_descriptor_uuid}){
    if(!id.is_nil()&&!core::uuid::IsEngineIdentityUuid(id))return false;
    payload->insert(payload->end(),id.bytes.begin(),id.bytes.end());
  }
  for (const auto& field : DomainStringFields(record)) {
    if (!AppendLengthPrefixedString(payload, field)) { return false; }
  }
  return true;
}

bool DeserializeDomainPayload(const std::vector<byte>& payload, DomainRecord* record) {
  std::vector<std::string> fields;
  fields.reserve(kDomainStringFieldCount);
  std::size_t offset=0;
  for(auto* id:{&record->domain_uuid,&record->catalog_row_uuid,&record->schema_uuid,&record->base_descriptor_uuid}){
    if(payload.size()-offset<16)return false;
    std::copy_n(payload.begin()+offset,16,id->bytes.begin());offset+=16;
    if(!id->is_nil()&&!core::uuid::IsEngineIdentityUuid(*id))return false;
  }
  if(record->domain_uuid.is_nil())return false;
  for (u32 i = 0; i < kDomainStringFieldCount; ++i) {
    std::string field;
    if (!ReadLengthPrefixedString(payload, &offset, &field)) { return false; }
    fields.push_back(std::move(field));
  }
  if (offset != payload.size()) { return false; }
  return AssignDomainStringFields(fields, record);
}

bool EncodeBinaryDomainRecord(const BinaryDomainRecord& input,
                              std::vector<byte>* encoded) {
  std::vector<byte> payload;
  if (!SerializeDomainPayload(input.record, &payload)) { return false; }
  if (payload.size() > std::numeric_limits<u64>::max()) { return false; }

  std::vector<byte> record(kDomainCatalogRecordHeaderBytes);
  std::copy(kDomainCatalogRecordMagic.begin(), kDomainCatalogRecordMagic.end(), record.begin());
  Store16(&record, kRecordOffsetVersion, kDomainCatalogVersion);
  Store16(&record, kRecordOffsetHeaderBytes, kDomainCatalogRecordHeaderBytes);
  Store16(&record, kRecordOffsetAction, static_cast<u16>(input.action));
  u32 flags = 0;
  if (input.record.nullable) { flags |= kDomainRecordFlagNullable; }
  if (input.record.dropped || input.action == DomainBinaryAction::drop) {
    flags |= kDomainRecordFlagDropped;
  }
  Store32(&record, kRecordOffsetFlags, flags);
  Store64(&record, kRecordOffsetSequence, input.sequence);
  Store64(&record, kRecordOffsetCreatorTx, input.record.creator_tx);
  Store64(&record, kRecordOffsetPayloadBytes, static_cast<u64>(payload.size()));
  Store16(&record, kRecordOffsetPayloadDigestBytes, kDomainCatalogDigestBytes);
  record.insert(record.end(), payload.begin(), payload.end());
  if (!AttachSha256Digest(&record, kRecordOffsetDigest, kDomainCatalogDigestBytes)) {
    return false;
  }
  encoded->insert(encoded->end(), record.begin(), record.end());
  return true;
}

bool DecodeBinaryDomainRecord(const std::vector<byte>& catalog,
                              std::size_t* offset,
                              u64* last_sequence,
                              BinaryDomainRecord* out,
                              std::string* detail) {
  if (*offset > catalog.size() ||
      catalog.size() - *offset < kDomainCatalogRecordHeaderBytes) {
    *detail = "domain_catalog_record_truncated";
    return false;
  }
  const std::size_t record_start = *offset;
  if (!MagicEquals(catalog, record_start, kDomainCatalogRecordMagic)) {
    *detail = "domain_catalog_record_magic_invalid";
    return false;
  }
  const u16 version = Load16(catalog, record_start + kRecordOffsetVersion);
  const u16 header_bytes = Load16(catalog, record_start + kRecordOffsetHeaderBytes);
  const auto action = static_cast<DomainBinaryAction>(
      Load16(catalog, record_start + kRecordOffsetAction));
  const u32 flags = Load32(catalog, record_start + kRecordOffsetFlags);
  const u64 sequence = Load64(catalog, record_start + kRecordOffsetSequence);
  const u64 creator_tx = Load64(catalog, record_start + kRecordOffsetCreatorTx);
  const u64 payload_bytes = Load64(catalog, record_start + kRecordOffsetPayloadBytes);
  const u16 digest_bytes = Load16(catalog, record_start + kRecordOffsetPayloadDigestBytes);
  if (version != kDomainCatalogVersion ||
      header_bytes != kDomainCatalogRecordHeaderBytes ||
      digest_bytes != kDomainCatalogDigestBytes) {
    *detail = "domain_catalog_record_header_invalid";
    return false;
  }
  if (action != DomainBinaryAction::create &&
      action != DomainBinaryAction::alter &&
      action != DomainBinaryAction::drop) {
    *detail = "domain_catalog_record_action_invalid";
    return false;
  }
  if (sequence == 0 || sequence <= *last_sequence) {
    *detail = "domain_catalog_record_sequence_invalid";
    return false;
  }
  if (payload_bytes > std::numeric_limits<std::size_t>::max() ||
      catalog.size() - record_start - header_bytes < payload_bytes) {
    *detail = "domain_catalog_record_payload_truncated";
    return false;
  }
  const std::size_t record_bytes = header_bytes + static_cast<std::size_t>(payload_bytes);
  std::vector<byte> record(catalog.begin() + static_cast<std::ptrdiff_t>(record_start),
                           catalog.begin() + static_cast<std::ptrdiff_t>(record_start + record_bytes));
  if (!Sha256DigestMatches(record, kRecordOffsetDigest, digest_bytes)) {
    *detail = "domain_catalog_record_digest_mismatch";
    return false;
  }
  const std::vector<byte> payload(
      catalog.begin() + static_cast<std::ptrdiff_t>(record_start + header_bytes),
      catalog.begin() + static_cast<std::ptrdiff_t>(record_start + record_bytes));
  DomainRecord record_payload;
  if (!DeserializeDomainPayload(payload, &record_payload)) {
    *detail = "domain_catalog_record_payload_invalid";
    return false;
  }
  if (record_payload.creator_tx != 0 && record_payload.creator_tx != creator_tx) {
    *detail = "domain_catalog_record_creator_tx_conflict";
    return false;
  }
  record_payload.creator_tx = creator_tx;
  record_payload.nullable = (flags & kDomainRecordFlagNullable) != 0;
  record_payload.dropped = action == DomainBinaryAction::drop ||
                           (flags & kDomainRecordFlagDropped) != 0;
  out->action = action;
  out->sequence = sequence;
  out->record = std::move(record_payload);
  *last_sequence = sequence;
  *offset = record_start + record_bytes;
  return true;
}

BinaryCatalogLoadResult LoadBinaryDomainCatalog(const EngineRequestContext& context) {
  const std::string path = DomainCatalogPath(context);
  std::error_code ec;
  const bool present = std::filesystem::exists(path, ec);
  if (ec) { return BinaryCatalogError("domain.load_state", "domain_catalog_stat_failed:" + ec.message()); }
  if (!present) { return BinaryCatalogAbsent(); }

  std::ifstream in(path, std::ios::binary);
  if (!in) { return BinaryCatalogError("domain.load_state", "domain_catalog_open_failed"); }
  std::vector<byte> encoded((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
  if (!in.eof() && in.bad()) {
    return BinaryCatalogError("domain.load_state", "domain_catalog_read_failed");
  }
  if (encoded.size() < kDomainCatalogHeaderBytes ||
      !MagicEquals(encoded, 0, kDomainCatalogMagic)) {
    return BinaryCatalogError("domain.load_state", "domain_catalog_header_invalid");
  }
  const u16 version = Load16(encoded, kCatalogOffsetVersion);
  const u16 header_bytes = Load16(encoded, kCatalogOffsetHeaderBytes);
  const u64 generation = Load64(encoded, kCatalogOffsetGeneration);
  const u64 record_count = Load64(encoded, kCatalogOffsetRecordCount);
  const u64 record_bytes = Load64(encoded, kCatalogOffsetRecordBytes);
  const u16 digest_bytes = Load16(encoded, kCatalogOffsetDigestBytes);
  if (version != kDomainCatalogVersion ||
      header_bytes != kDomainCatalogHeaderBytes ||
      digest_bytes != kDomainCatalogDigestBytes) {
    return BinaryCatalogError("domain.load_state", "domain_catalog_header_invalid");
  }
  if (record_bytes > std::numeric_limits<std::size_t>::max() ||
      encoded.size() != static_cast<std::size_t>(header_bytes) +
                            static_cast<std::size_t>(record_bytes)) {
    return BinaryCatalogError("domain.load_state", "domain_catalog_size_mismatch");
  }
  if (record_count > 0 &&
      record_count > record_bytes / kDomainCatalogRecordHeaderBytes) {
    return BinaryCatalogError("domain.load_state", "domain_catalog_record_count_invalid");
  }
  if (!Sha256DigestMatches(encoded, kCatalogOffsetDigest, digest_bytes)) {
    return BinaryCatalogError("domain.load_state", "domain_catalog_digest_mismatch");
  }

  BinaryCatalogLoadResult result;
  result.ok = true;
  result.present = true;
  result.records.reserve(static_cast<std::size_t>(std::min<u64>(
      record_count, static_cast<u64>(std::numeric_limits<std::size_t>::max()))));
  std::size_t offset = header_bytes;
  u64 last_sequence = 0;
  for (u64 i = 0; i < record_count; ++i) {
    BinaryDomainRecord record;
    std::string detail;
    if (!DecodeBinaryDomainRecord(encoded, &offset, &last_sequence, &record, &detail)) {
      return BinaryCatalogError("domain.load_state", detail);
    }
    result.records.push_back(std::move(record));
  }
  if (offset != encoded.size()) {
    return BinaryCatalogError("domain.load_state", "domain_catalog_trailing_bytes");
  }
  if ((record_count == 0 && generation != 0) ||
      (record_count > 0 && generation != last_sequence)) {
    return BinaryCatalogError("domain.load_state", "domain_catalog_generation_mismatch");
  }
  return result;
}

std::vector<byte> EncodeBinaryDomainCatalog(const std::vector<BinaryDomainRecord>& records) {
  std::vector<byte> record_bytes;
  u64 generation = 0;
  for (const auto& record : records) {
    if (record.sequence <= generation) { return {}; }
    if (!EncodeBinaryDomainRecord(record, &record_bytes)) { return {}; }
    generation = record.sequence;
  }

  std::vector<byte> encoded(kDomainCatalogHeaderBytes);
  std::copy(kDomainCatalogMagic.begin(), kDomainCatalogMagic.end(), encoded.begin());
  Store16(&encoded, kCatalogOffsetVersion, kDomainCatalogVersion);
  Store16(&encoded, kCatalogOffsetHeaderBytes, kDomainCatalogHeaderBytes);
  Store32(&encoded, kCatalogOffsetFlags, 0);
  Store64(&encoded, kCatalogOffsetGeneration, generation);
  Store64(&encoded, kCatalogOffsetRecordCount, static_cast<u64>(records.size()));
  Store64(&encoded, kCatalogOffsetRecordBytes, static_cast<u64>(record_bytes.size()));
  Store16(&encoded, kCatalogOffsetDigestBytes, kDomainCatalogDigestBytes);
  encoded.insert(encoded.end(), record_bytes.begin(), record_bytes.end());
  if (!AttachSha256Digest(&encoded, kCatalogOffsetDigest, kDomainCatalogDigestBytes)) {
    return {};
  }
  return encoded;
}

bool ReplaceDomainCatalogAtomically(const std::filesystem::path& temp_path,
                                    const std::filesystem::path& target_path,
                                    std::string* detail) {
#if defined(_WIN32)
  if (::MoveFileExW(temp_path.wstring().c_str(),
                    target_path.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
    return true;
  }
  if (detail != nullptr) {
    *detail = "win32_error=" + std::to_string(::GetLastError());
  }
  return false;
#else
  std::error_code ec;
  std::filesystem::rename(temp_path, target_path, ec);
  if (!ec) { return true; }
  if (detail != nullptr) { *detail = ec.message(); }
  return false;
#endif
}

EngineApiDiagnostic PersistBinaryDomainCatalog(const EngineRequestContext& context,
                                               const std::vector<BinaryDomainRecord>& records) {
  const std::vector<byte> encoded = EncodeBinaryDomainCatalog(records);
  if (encoded.empty()) {
    return DomainCatalogDiagnostic("domain.append_event", "domain_catalog_encode_failed");
  }
  const std::filesystem::path target_path = DomainCatalogPath(context);
  const std::filesystem::path temp_path = target_path.string() + ".tmp";
  std::error_code ec;
  const bool temp_present = std::filesystem::exists(temp_path, ec);
  if (ec) {
    return DomainCatalogDiagnostic("domain.append_event",
                                   "domain_catalog_stale_temp_stat_failed:" + ec.message());
  }
  if (temp_present) {
    std::filesystem::remove(temp_path, ec);
    if (ec) {
      return DomainCatalogDiagnostic("domain.append_event",
                                     "domain_catalog_stale_temp_remove_failed:" + ec.message());
    }
    const auto parent_sync = disk::SyncParentDirectoryPath(temp_path.string());
    if (!parent_sync.ok()) {
      return DomainCatalogDiagnostic("domain.append_event",
                                     "domain_catalog_parent_sync_failed:" +
                                         parent_sync.diagnostic.diagnostic_code);
    }
  }

  {
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      return DomainCatalogDiagnostic("domain.append_event", "domain_catalog_temp_open_failed");
    }
    out.write(reinterpret_cast<const char*>(encoded.data()),
              static_cast<std::streamsize>(encoded.size()));
    out.close();
    if (!out) {
      return DomainCatalogDiagnostic("domain.append_event", "domain_catalog_temp_write_failed");
    }
  }

  const auto file_sync = disk::SyncFilesystemPath(temp_path.string(), true);
  if (!file_sync.ok()) {
    return DomainCatalogDiagnostic("domain.append_event",
                                   "domain_catalog_file_sync_failed:" +
                                       file_sync.diagnostic.diagnostic_code);
  }
  std::string replace_detail;
  if (!ReplaceDomainCatalogAtomically(temp_path, target_path, &replace_detail)) {
    return DomainCatalogDiagnostic("domain.append_event",
                                   "domain_catalog_rename_failed:" + replace_detail);
  }
  const auto parent_sync = disk::SyncParentDirectoryPath(target_path.string());
  if (!parent_sync.ok()) {
    return DomainCatalogDiagnostic("domain.append_event",
                                   "domain_catalog_parent_sync_failed:" +
                                       parent_sync.diagnostic.diagnostic_code);
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

BinaryCatalogLoadResult RejectLegacyDomainEvents(const EngineRequestContext& context){
  std::error_code error;const bool legacy=std::filesystem::exists(DomainEventPath(context),error);
  if(legacy||error)return BinaryCatalogError("domain.load_state","legacy_domain_format_unsupported");
  return BinaryCatalogAbsent();
}

EngineApiDiagnostic ValidateDomainMutatingTransactionAuthority(const EngineRequestContext& context,
                                                               const std::string& event) {
  if (context.local_transaction_id == 0) {
    return MakeInvalidRequestDiagnostic("domain.append_event", "local_transaction_id_required");
  }
  const auto crud = LoadCrudState(context);
  if (!crud.ok) {
    return crud.diagnostic.error
               ? crud.diagnostic
               : MakeInvalidRequestDiagnostic("domain.append_event", "transaction_authority_unavailable");
  }
  const auto tx = crud.state.transactions.find(context.local_transaction_id);
  if (tx == crud.state.transactions.end() || tx->second != "active") {
    return MakeInvalidRequestDiagnostic("domain.append_event", "active_local_transaction_required");
  }
  DomainBinaryAction action;DomainRecord record;
  if(!DecodeDomainEvent(event,&action,&record))return MakeInvalidRequestDiagnostic("domain.append_event","domain_event_invalid");
  const auto creator_tx=record.creator_tx;
  if (creator_tx != context.local_transaction_id) {
    return MakeInvalidRequestDiagnostic("domain.append_event", "domain_creator_tx_mismatch");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

void AppendEncodedDescriptorField(std::string* descriptor,
                                  const std::string& key,
                                  const std::string& value) {
  if (value.empty()) { return; }
  descriptor->append(";");
  descriptor->append(key);
  descriptor->append("=");
  descriptor->append(EncodeCrudText(value));
}

}  // namespace

DomainStoreResult LoadDomainState(const EngineRequestContext& context) {
  DomainStoreResult result;
  const auto path_status = ValidateCrudDatabasePath(context, "domain.load_state");
  if (path_status.error) { result.diagnostic = path_status; return result; }
  auto loaded = LoadBinaryDomainCatalog(context);
  if (!loaded.ok) {
    result.diagnostic = loaded.diagnostic;
    return result;
  }
  if (!loaded.present) {
    loaded = RejectLegacyDomainEvents(context);
    if (!loaded.ok) {
      result.diagnostic = loaded.diagnostic;
      return result;
    }
  }
  for (auto& record : loaded.records) {
    result.domains.push_back(std::move(record.record));
  }
  result.ok = true;
  return result;
}

EngineApiDiagnostic AppendDomainEvent(const EngineRequestContext& context, const std::string& event) {
  const auto inventory_guard = AcquireTransactionInventoryGuard(context.database_path);
  const auto path_status = ValidateCrudDatabasePath(context, "domain.append_event");
  if (path_status.error) { return path_status; }
  const auto authority_status = ValidateDomainMutatingTransactionAuthority(context, event);
  if (authority_status.error) { return authority_status; }
  BinaryDomainRecord new_record;
  if (!DecodeDomainEvent(event, &new_record.action, &new_record.record)) {
    return MakeInvalidRequestDiagnostic("domain.append_event", "domain_event_invalid");
  }
  if (new_record.record.creator_tx != context.local_transaction_id) {
    return MakeInvalidRequestDiagnostic("domain.append_event", "domain_creator_tx_mismatch");
  }

  auto loaded = LoadBinaryDomainCatalog(context);
  if (!loaded.ok) { return loaded.diagnostic; }
  if (!loaded.present) {
    loaded = RejectLegacyDomainEvents(context);
    if (!loaded.ok) { return loaded.diagnostic; }
  }
  u64 sequence = 0;
  for (const auto& record : loaded.records) {
    sequence = std::max(sequence, record.sequence);
  }
  new_record.sequence = sequence + 1;
  new_record.record.dropped = new_record.action == DomainBinaryAction::drop;
  loaded.records.push_back(std::move(new_record));
  return PersistBinaryDomainCatalog(context, loaded.records);
}

std::string MakeDomainCreateEvent(const DomainRecord& record) {
  return MakeDomainEvent("DOMAIN_CREATE", record);
}

std::string MakeDomainAlterEvent(const DomainRecord& record) {
  return MakeDomainEvent("DOMAIN_ALTER", record);
}

std::string MakeDomainDropEvent(std::uint64_t creator_tx, const EngineUuid& domain_uuid) {
  DomainRecord record;record.creator_tx=creator_tx;record.domain_uuid=domain_uuid;record.dropped=true;return MakeDomainEvent("DOMAIN_DROP",record);
}

std::optional<DomainRecord> FindVisibleDomain(const EngineRequestContext& context,
                                              const EngineUuid& domain_uuid,
                                              std::uint64_t observer_tx) {
  const auto inventory_guard = AcquireTransactionInventoryGuard(context.database_path);
  const auto domains = LoadDomainState(context);
  if (!domains.ok) { return std::nullopt; }
  const auto crud = LoadCrudState(context);
  if (!crud.ok) { return std::nullopt; }
  std::optional<DomainRecord> visible;
  for (const auto& domain : domains.domains) {
    if (domain.domain_uuid == domain_uuid && TxVisible(crud.state, domain.creator_tx, observer_tx)) {
      if (domain.dropped) {
        visible.reset();
      } else {
        visible = domain;
      }
    }
  }
  return visible;
}

EngineApiDiagnostic BindDomainScalarBaseDescriptor(
    const EngineRequestContext& context, const EngineDescriptor& descriptor,
    DomainRecord* record) {
  std::string detail;
  if (!record || !AdmitDomainScalarBaseDescriptor(context, descriptor, &detail))
    return DomainValidationDiagnostic(record ? detail : "domain_record_required");
  std::string encoded;
  const auto registered = dt::LookupDatatypeTypeCodecIdentityV3(
      context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
      context.datatype_registry_generation, descriptor.datatype_descriptor_uuid,
      descriptor.datatype_descriptor_generation);
  if (!registered.ok || !EncodeDomainInheritedBaseBindingV1(
          CaptureDomainInheritedBaseBindingV1(descriptor, registered.row), &encoded))
    return DomainValidationDiagnostic("domain_base_descriptor_encoding_failed");
  auto staged = *record;
  staged.base_descriptor_uuid = descriptor.datatype_descriptor_uuid;
  staged.base_descriptor_kind = descriptor.descriptor_kind;
  staged.base_canonical_type_name = descriptor.canonical_type_name;
  staged.base_encoded_descriptor = std::move(encoded);
  *record = std::move(staged);
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic BindDomainInnerBaseDescriptor(
    const EngineRequestContext& context, const DomainRecord& inner,
    DomainRecord* record) {
  if (!record) return DomainValidationDiagnostic("domain_record_required");
  const auto inventory_guard = AcquireTransactionInventoryGuard(context.database_path);
  const auto visible = FindVisibleDomain(context, inner.domain_uuid, context.local_transaction_id);
  if (!visible || MakeDomainCreateEvent(*visible) != MakeDomainCreateEvent(inner))
    return DomainValidationDiagnostic("domain_inner_binding_not_current");
  EngineDescriptor base;
  std::string detail;
  if (!ResolveDomainScalarBaseDescriptor(context, inner, context.local_transaction_id, &base, &detail))
    return DomainValidationDiagnostic(detail);
  const auto registered = dt::LookupDatatypeTypeCodecIdentityV3(
      context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
      context.datatype_registry_generation, base.datatype_descriptor_uuid, base.datatype_descriptor_generation);
  std::string encoded;
  if (!registered.ok || !EncodeDomainInheritedBaseBindingV1(
        CaptureDomainInheritedBaseBindingV1(base, registered.row), &encoded))
    return DomainValidationDiagnostic("domain_inherited_profile_binding_invalid");
  auto staged = *record;
  staged.base_descriptor_uuid = inner.domain_uuid;
  staged.base_descriptor_kind = "domain";
  staged.base_canonical_type_name = base.canonical_type_name;
  staged.base_encoded_descriptor = std::move(encoded);
  *record = std::move(staged);
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

DomainInheritedProfileResolution ResolveDomainInheritedProfile(
    const EngineRequestContext& context, const EngineUuid& domain_uuid,
    std::uint64_t observer_tx) {
  const auto inventory_guard = AcquireTransactionInventoryGuard(context.database_path);
  DomainInheritedProfileResolution result;
  const auto domain = FindVisibleDomain(context, domain_uuid, observer_tx);
  std::string detail;
  std::vector<DomainRecord> chain;
  if (!domain || !ResolveDomainScalarBaseDescriptor(context, *domain, observer_tx,
          &result.base_descriptor, &detail, &result.binary_chain_snapshot, &chain)) {
    result.diagnostic = DomainValidationDiagnostic(domain ? detail : "domain_not_visible");
    return result;
  }
  result.nullable_allowed = true;
  for (const auto& record : chain) {
    const auto* current = &record;
    // These legacy metadata carriers cannot authorize a custom semantic
    // profile. Their presence is not permission to discard an override.
    if (!current->method_binding_envelope.empty() || !current->numeric_metadata.empty() ||
        !current->charset_or_collation_ref.empty()) {
      result.diagnostic = MakeEngineApiDiagnostic("DOMAIN.PROFILE_INCOMPLETE",
          "domain.profile.incomplete", "domain explicit semantic binding must be resolved before index inheritance", true);
      return result;
    }
    result.domain_chain.push_back(current->domain_uuid);
    result.nullable_allowed = result.nullable_allowed && current->nullable;
    if (current->base_descriptor_kind != "domain") {
      result.binary_profile_binding = current->base_encoded_descriptor;
    }
  }
  result.ok = true;
  result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return result;
}

EngineApiDiagnostic AdmitDomainMutationChain(const EngineRequestContext& context,
    const EngineUuid& domain_uuid, std::uint64_t observer_tx) {
  const auto inventory_guard = AcquireTransactionInventoryGuard(context.database_path);
  const auto domain = FindVisibleDomain(context, domain_uuid, observer_tx);
  EngineDescriptor base;
  std::string detail;
  std::vector<DomainRecord> chain;
  if (!domain || !ResolveDomainScalarBaseDescriptor(context, *domain, observer_tx, &base, &detail, nullptr, &chain))
    return DomainValidationDiagnostic(domain ? detail : "domain_not_visible");
  for (const auto& current : chain) {
    for (const auto* policy : {&current.mutation_policy_envelope, &current.cast_policy_envelope}) {
      if (policy->empty()) continue;
      const auto required = RequiredRightFromPolicy(*policy);
      if (required.empty()) return DomainValidationDiagnostic("domain_restriction_policy_invalid");
      if (!HasDomainRight(context, current.domain_uuid, required))
        return DomainValidationDiagnostic("domain_use_right_denied:" + required);
    }
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineDescriptor DomainDescriptor(const DomainRecord& record) {
  EngineDescriptor descriptor;
  descriptor.descriptor_uuid = record.domain_uuid;
  descriptor.descriptor_kind = "domain";
  descriptor.canonical_type_name = record.default_name.empty() ? record.base_canonical_type_name : record.default_name;
  descriptor.encoded_descriptor = std::string("base_type=")+record.base_canonical_type_name+
                                  ";nullable=" + (record.nullable ? "true" : "false") +
                                  ";validation_hook_status=" + record.validation_hook_status;
  AppendEncodedDescriptorField(&descriptor.encoded_descriptor, "cast_policy", record.cast_policy_envelope);
  AppendEncodedDescriptorField(&descriptor.encoded_descriptor, "mutation_policy", record.mutation_policy_envelope);
  AppendEncodedDescriptorField(&descriptor.encoded_descriptor, "masking_policy", record.masking_policy_envelope);
  AppendEncodedDescriptorField(&descriptor.encoded_descriptor, "visibility_policy", record.visibility_policy_envelope);
  AppendEncodedDescriptorField(&descriptor.encoded_descriptor, "encryption_policy", record.encryption_policy_ref);
  AppendEncodedDescriptorField(&descriptor.encoded_descriptor, "driver_metadata", record.driver_metadata_envelope);
  AppendEncodedDescriptorField(&descriptor.encoded_descriptor, "wire_metadata", record.wire_metadata_envelope);
  AppendEncodedDescriptorField(&descriptor.encoded_descriptor, "element_path", record.element_path_envelope);
  AppendEncodedDescriptorField(&descriptor.encoded_descriptor, "method_binding", record.method_binding_envelope);
  AppendEncodedDescriptorField(&descriptor.encoded_descriptor, "localized_names", record.localized_names_envelope);
  AppendEncodedDescriptorField(&descriptor.encoded_descriptor, "comment", record.comment_envelope);
  AppendEncodedDescriptorField(&descriptor.encoded_descriptor, "reference_aliases", record.reference_alias_envelope);
  return descriptor;
}

EngineUuid DomainUuidFromDescriptor(const EngineDescriptor& descriptor) {
  if (descriptor.descriptor_kind == "domain" && !descriptor.descriptor_uuid.is_nil()) {
    return descriptor.descriptor_uuid;
  }
  return DomainUuidFromColumnDescriptor(descriptor.encoded_descriptor);
}

std::string DomainColumnDescriptor(const EngineUuid& id) {
  if (!core::uuid::IsEngineIdentityUuid(id)) return {};
  CatalogColumnMetadata fields;
  fields.identities.emplace("domain_uuid", id);
  std::string bytes;
  return EncodeCatalogColumnMetadata(fields, &bytes) ? bytes : std::string{};
}
EngineUuid DomainUuidFromColumnDescriptor(const std::string& bytes) {
  CatalogColumnMetadata fields;
  if (!AdmitCatalogColumnMetadata(bytes, &fields)) return {};
  const auto id = BinaryCatalogUuid(fields, "domain_uuid");
  return core::uuid::IsEngineIdentityUuid(id) ? id : EngineUuid{};
}


bool IsSupportedDomainCheckEnvelope(const std::string& envelope) {
  if (envelope.empty()) { return true; }
  if (StartsWith(envelope, "sblr_predicate:")) {
    return IsSupportedDomainCheckPredicate(envelope.substr(15));
  }
  return IsSupportedDomainCheckPredicate(envelope);
}

bool DomainChainContainsUuid(const EngineRequestContext& context,
                             const EngineUuid& start_domain_uuid,
                             const EngineUuid& searched_domain_uuid,
                             std::uint64_t observer_tx) {
  if (start_domain_uuid.is_nil() || searched_domain_uuid.is_nil()) { return false; }
  const auto guard = AcquireTransactionInventoryGuard(context.database_path);
  const auto catalog = LoadDomainState(context);
  const auto crud = LoadCrudState(context);
  // This admission predicate must not permit a dependency on unreadable state.
  if (!catalog.ok || !crud.ok) return true;
  std::map<EngineUuid, const DomainRecord*> visible;
  for (const auto& record : catalog.domains) {
    if (!TxVisible(crud.state, record.creator_tx, observer_tx)) continue;
    if (record.dropped) visible.erase(record.domain_uuid);
    else visible[record.domain_uuid] = &record;
  }
  EngineUuid current = start_domain_uuid;
  std::set<EngineUuid> seen;
  while (!current.is_nil()) {
    if (current == searched_domain_uuid) { return true; }
    if (!seen.insert(current).second) return true;
    const auto found = visible.find(current);
    if (found == visible.end()) return true;
    const auto& domain = *found->second;
    current = domain.base_descriptor_kind == "domain" ? domain.base_descriptor_uuid : EngineUuid{};
  }
  return false;
}

DomainValueValidationResult ValidateDomainTypedValue(const EngineRequestContext& context,
                                                     const EngineDescriptor& domain_descriptor,
                                                     const EngineTypedValue& input_value,
                                                     std::uint64_t observer_tx,
                                                     bool explicit_cast) {
  const auto inventory_guard = AcquireTransactionInventoryGuard(context.database_path);
  DomainValueValidationResult result;
  if (context.query_cancellation_requested && context.query_cancellation_requested()) {
    result.diagnostic = MakeEngineApiDiagnostic("PROCESS.CANCELLED", "domain.validation.cancelled", {}, true);
    return result;
  }
  const EngineUuid domain_uuid = DomainUuidFromDescriptor(domain_descriptor);
  if (domain_uuid.is_nil()) {
    result.diagnostic = DomainValidationDiagnostic("domain_uuid_required");
    return result;
  }
  const auto domain = FindVisibleDomain(context, domain_uuid, observer_tx);
  if (!domain) {
    result.diagnostic = DomainValidationDiagnostic("domain_not_visible");
    return result;
  }
  EngineDescriptor native_base;
  std::string binding_detail;
  std::vector<DomainRecord> target_chain;
  if (!ResolveDomainScalarBaseDescriptor(context, *domain, observer_tx,
                                         &native_base, &binding_detail, nullptr, &target_chain)) {
    result.diagnostic = DomainValidationDiagnostic(binding_detail);
    return result;
  }
  if (input_value.isSqlNull()) {
    if (!QowCanonicalSqlNullStateV1(input_value)) {
      result.diagnostic = DomainValidationDiagnostic("domain_null_payload_invalid");
      return result;
    }
  }
  if ((input_value.state != EngineValueState::value && !input_value.isSqlNull()) ||
      (!input_value.binary_value.empty() &&
       !input_value.encoded_value.empty())) {
    result.diagnostic = DomainValidationDiagnostic("domain_value_encoding_invalid");
    return result;
  }
  const auto admit_restrictions = [&](const std::vector<DomainRecord>& chain) {
    for (const auto& item : chain) {
      if (!item.method_binding_envelope.empty() || !item.numeric_metadata.empty() ||
          !item.charset_or_collation_ref.empty()) {
        result.diagnostic = MakeEngineApiDiagnostic("DOMAIN.PROFILE_INCOMPLETE",
            "domain.profile.incomplete", "domain explicit semantic binding must be resolved before native validation", true);
        return false;
      }
      const auto required = RequiredRightFromPolicy(item.cast_policy_envelope);
      if (!item.cast_policy_envelope.empty() && required.empty()) {
        result.diagnostic = DomainValidationDiagnostic("domain_restriction_policy_invalid");return false;
      }
      if (!required.empty() && !HasDomainRight(context,item.domain_uuid,required)) {
        result.diagnostic = DomainValidationDiagnostic("domain_cast_right_denied:" + required);return false;
      }
      if (input_value.isSqlNull() && !item.nullable) {
        result.diagnostic = DomainValidationDiagnostic("domain_null_forbidden");return false;
      }
    }
    return true;
  };
  const auto check_chain = [&](const std::vector<DomainRecord>& chain, const EngineDescriptor& base,
                               const EngineTypedValue& value, bool source) {
    const auto payload=CrudTypedValuePayload(value);
    for (auto item=chain.rbegin(); item!=chain.rend(); ++item) {
      std::string detail;
      if (!value.isSqlNull() && !CheckConstraintPasses(item->check_constraint_envelope,base,payload.bytes,&detail)) {
        result.diagnostic=DomainValidationDiagnostic(detail);return false;
      }
      result.evidence.push_back({source?"source_domain_validation":"domain_validation",item->domain_uuid});
      if (!value.isSqlNull() && !item->check_constraint_envelope.empty())
        result.evidence.push_back({source?"source_domain_check":"domain_check",item->domain_uuid});
      if (!source && item->domain_uuid!=domain_uuid)
        result.evidence.push_back({"domain_base_validation",item->domain_uuid});
    }
    return true;
  };
  if (!admit_restrictions(target_chain)) return result;
  EngineTypedValue value_for_base_cast = input_value;
  if (input_value.descriptor.descriptor_kind == "domain") {
    const auto source_domain = FindVisibleDomain(context,
        DomainUuidFromDescriptor(input_value.descriptor), observer_tx);
    std::vector<DomainRecord> source_chain;
    if (!source_domain || input_value.descriptor != DomainDescriptor(*source_domain) ||
        !ResolveDomainScalarBaseDescriptor(context, *source_domain, observer_tx,
                                            &value_for_base_cast.descriptor, &binding_detail, nullptr, &source_chain)) {
      result.diagnostic = DomainValidationDiagnostic("domain_source_binding_invalid:" + binding_detail);
      return result;
    }
    if (!admit_restrictions(source_chain)) return result;
    EngineTypedValue source_checked;
    std::string category;
    if (!QowApplyCanonicalDescriptorCoercionV1(value_for_base_cast, value_for_base_cast.descriptor,
          true, &source_checked, &category, &binding_detail)) {
      result.diagnostic=DomainValidationDiagnostic("domain_source_value_invalid:"+binding_detail);
      return result;
    }
    if (source_checked.state!=input_value.state || source_checked.binary_value!=input_value.binary_value ||
        source_checked.encoded_value!=input_value.encoded_value) {
      result.diagnostic=DomainValidationDiagnostic("domain_source_validation_changed_payload");return result;
    }
    if (!check_chain(source_chain, value_for_base_cast.descriptor,source_checked,true)) return result;
    value_for_base_cast=std::move(source_checked);
  }
  if (!AdmitDomainScalarBaseDescriptor(context, value_for_base_cast.descriptor,
                                       &binding_detail)) {
    result.diagnostic = DomainValidationDiagnostic("domain_source_binding_invalid:" + binding_detail);
    return result;
  }
  EngineTypedValue cast;
  std::string category;
  if (!QowApplyCanonicalDescriptorCoercionV1(value_for_base_cast, native_base, explicit_cast,
                                            &cast, &category, &binding_detail)) {
    result.diagnostic = DomainValidationDiagnostic("domain_base_cast_failed:" + binding_detail);
    return result;
  }
  if (!check_chain(target_chain,native_base,cast,false)) return result;
  result.ok = true;
  result.value = std::move(cast);
  result.value.descriptor = DomainDescriptor(*domain);
  return result;
}

DomainRowValidationResult ApplyDomainRulesToCrudValues(
    const EngineRequestContext& context,
    const std::vector<std::pair<std::string, std::string>>& table_columns,
    const CrudValueFields& input_values,
    std::uint64_t observer_tx,
    ConstraintDmlValidationCache* cache) {
  const auto inventory_guard = AcquireTransactionInventoryGuard(context.database_path);
  DomainRowValidationResult result;
  result.values = input_values;
  for (const auto& [column_name, column_descriptor] : table_columns) {
    const EngineUuid domain_uuid = DomainUuidFromColumnDescriptor(column_descriptor);
    if(domain_uuid.is_nil()&&(column_descriptor.starts_with("domain:")||column_descriptor.starts_with("SBDOMID2")||!DescriptorField(column_descriptor,"domain_uuid").empty())){
      result.diagnostic=DomainValidationDiagnostic("domain_binding_format_invalid");return result;
    }
    if (domain_uuid.is_nil()) { continue; }
    const auto domain = FindVisibleDomain(context, domain_uuid, observer_tx);
    if (!domain) {
      result.diagnostic = DomainValidationDiagnostic("domain_not_visible_for_column:" + column_name);
      return result;
    }
    const auto mutation = AdmitDomainMutationChain(context, domain_uuid, observer_tx);
    if (mutation.error) {
      result.diagnostic = mutation;
      return result;
    }
    const bool present = HasField(result.values, column_name);
    bool literal_default = false;
    if (!present || FieldValue(result.values, column_name).isDefaultRequested()) {
      const auto default_value = MaterializeDefault(domain->default_expression_envelope);
      if (default_value) {
        UpsertField(&result.values, column_name, *default_value);
        literal_default = true;
      } else if (!domain->default_expression_envelope.empty()) {
        result.diagnostic = DomainValidationDiagnostic("domain_default_expression_invalid:" + column_name);
        return result;
      } else if (!domain->nullable) {
        result.diagnostic = DomainValidationDiagnostic("domain_required_column_missing:" + column_name);
        return result;
      } else {
        UpsertField(&result.values, column_name, CrudStoredValue::SqlNull());
      }
    }
    EngineDescriptor descriptor = DomainDescriptor(*domain);
    EngineTypedValue value;
    std::string binding_detail;
    std::string domain_chain_material;
    if (!ResolveDomainScalarBaseDescriptor(context, *domain, observer_tx,
                                           &value.descriptor, &binding_detail, &domain_chain_material)) {
      result.diagnostic = DomainValidationDiagnostic(binding_detail);
      return result;
    }
    const auto stored_value = FieldValue(result.values, column_name);
    if (!stored_value.valid() || (!stored_value.isPresent() && !stored_value.isSqlNull())) {
      result.diagnostic = DomainValidationDiagnostic("domain_value_state_invalid:" + column_name);
      return result;
    }
    value.encoded_value = stored_value.bytes;
    value.setState(stored_value.state);
    std::string proof_identity("domain_check.v3");
    AppendBinaryString(&proof_identity, domain_chain_material);
    AppendBinaryU8(&proof_identity, literal_default ? 1 : 0);
    proof_identity.append(reinterpret_cast<const char*>(domain_uuid.bytes.data()),16);
    proof_identity.append(reinterpret_cast<const char*>(domain->base_descriptor_uuid.bytes.data()),16);
    for(const auto* field:std::initializer_list<const std::string*>{&column_name,&domain->base_descriptor_kind,&domain->base_canonical_type_name,&domain->check_constraint_envelope,&value.encoded_value})AppendBinaryString(&proof_identity,*field);
    AppendBinaryU8(&proof_identity,static_cast<std::uint8_t>(value.state));
    AppendBinaryU8(&proof_identity,domain->nullable?1:0);AppendBinaryU64(&proof_identity,observer_tx);
    if (const auto cached_value = FindConstraintDmlProofPayload(cache,
                                                                context,
                                                                "domain_check",
                                                                proof_identity,
                                                                &result.evidence)) {
      const auto cached_fields = DecodeCrudValues(*cached_value);
      if (!cached_fields || cached_fields->size() != 1 || cached_fields->front().first != column_name ||
          (!cached_fields->front().second.isPresent() && !cached_fields->front().second.isSqlNull())) {
        result.diagnostic = DomainValidationDiagnostic("domain_cached_value_state_invalid:" + column_name);
        return result;
      }
      UpsertField(&result.values, column_name, cached_fields->front().second);
      result.evidence.push_back({"domain_validation", domain_uuid});
      if (!domain->check_constraint_envelope.empty()) {
        result.evidence.push_back({"domain_check", domain_uuid});
      }
      continue;
    }
    if (literal_default) {
      if (!BindDomainLiteralCharacterDescriptor(context, &value.descriptor, &binding_detail)) {
        result.diagnostic = DomainValidationDiagnostic(binding_detail);
        return result;
      }
      value.encoded_value = stored_value.bytes;
      value.setState(stored_value.state);
    } else if (!RestoreStoredScalarPayloadV1(stored_value.bytes, stored_value.state, &value)) {
      result.diagnostic = DomainValidationDiagnostic("domain_stored_payload_invalid:" + column_name);
      return result;
    }
    const auto validation = ValidateDomainTypedValue(context, descriptor, value, observer_tx);
    if (!validation.ok) {
      result.diagnostic = validation.diagnostic;
      return result;
    }
    const auto current = FindVisibleDomain(context, domain_uuid, observer_tx);
    EngineDescriptor current_base;
    std::string current_material;
    if (!current || !ResolveDomainScalarBaseDescriptor(context, *current, observer_tx,
          &current_base, &binding_detail, &current_material) || current_material != domain_chain_material) {
      result.diagnostic = DomainValidationDiagnostic("domain_binding_changed_during_validation");
      return result;
    }
    const auto current_mutation = AdmitDomainMutationChain(context, domain_uuid, observer_tx);
    if (current_mutation.error) { result.diagnostic = current_mutation; return result; }
    const auto row_value = CrudTypedValuePayload(validation.value);
    UpsertField(&result.values, column_name, row_value);
    for (const auto& evidence : validation.evidence) { result.evidence.push_back(evidence); }
    StoreConstraintDmlProof(cache,
                            context,
                            "domain_check",
                            proof_identity,
                            EncodeCrudValues({{column_name, row_value}}),
                            &result.evidence);
  }
  result.ok = true;
  result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return result;
}

bool DomainHasCrudDependencies(const EngineRequestContext& context,
                               const EngineUuid& domain_uuid,
                               std::uint64_t observer_tx) {
  const auto crud = LoadCrudState(context);
  if (!crud.ok) { return true; }
  for (const auto& table : crud.state.tables) {
    if (!CrudCreatorVisible(crud.state, table.creator_tx, table.event_sequence, observer_tx)) { continue; }
    for (const auto& [ignored_name, descriptor] : table.columns) {
      if (DomainUuidFromColumnDescriptor(descriptor) == domain_uuid) { return true; }
    }
  }
  return false;
}

DomainReadPolicyResult ApplyDomainReadPoliciesToCrudValues(
    const EngineRequestContext& context,
    const std::vector<std::pair<std::string, std::string>>& table_columns,
    const CrudValueFields& input_values,
    std::uint64_t observer_tx) {
  DomainReadPolicyResult result;
  result.values = input_values;
  for (const auto& [column_name, column_descriptor] : table_columns) {
    const EngineUuid domain_uuid = DomainUuidFromColumnDescriptor(column_descriptor);
    if(domain_uuid.is_nil()&&(column_descriptor.starts_with("domain:")||column_descriptor.starts_with("SBDOMID2")||!DescriptorField(column_descriptor,"domain_uuid").empty())){
      result.diagnostic=DomainValidationDiagnostic("domain_binding_format_invalid");return result;
    }
    if (domain_uuid.is_nil()) { continue; }
    const auto domain = FindVisibleDomain(context, domain_uuid, observer_tx);
    if (!domain) {
      result.diagnostic = DomainValidationDiagnostic("domain_not_visible_for_column:" + column_name);
      return result;
    }
    std::string rejection_detail;
    if (!DomainVisibilityAllowsRead(context, *domain, column_name, &rejection_detail) ||
        !DomainEncryptionAllowsRead(context, *domain, column_name, &rejection_detail)) {
      result.diagnostic = DomainValidationDiagnostic(rejection_detail);
      return result;
    }
    result.evidence.push_back({"domain_read_policy", domain_uuid});
    if (!domain->masking_policy_envelope.empty()) {
      const auto value = FieldValue(result.values, column_name);
      if (HasField(result.values, column_name) && value.isPresent()) {
        const auto mask = ApplyDomainMaskPolicy(context, *domain, column_name, value.bytes);
        if (!mask.ok) {
          result.diagnostic = DomainValidationDiagnostic(mask.rejection_detail);
          return result;
        }
        UpsertField(&result.values, column_name, mask.value);
        if (mask.masked) { result.evidence.push_back({"domain_masking", domain_uuid}); }
        if (mask.unmasked) { result.evidence.push_back({"domain_unmask", domain_uuid}); }
      }
    }
  }
  result.ok = true;
  result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return result;
}

}  // namespace scratchbird::engine::internal_api
