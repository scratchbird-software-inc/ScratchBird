// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "domain_support/domain_store.hpp"

#include "crud_support/crud_store.hpp"
#include "datatype_operations.hpp"
#include "disk_device.hpp"
#include "dml/constraint_enforcement.hpp"
#include "hash_digest.hpp"
#include "runtime_platform.hpp"

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
    'S', 'B', 'D', 'O', 'M', 'C', '0', '1'};
inline constexpr std::array<byte, 8> kDomainCatalogRecordMagic = {
    'S', 'B', 'D', 'O', 'M', 'R', '0', '1'};
inline constexpr u16 kDomainCatalogVersion = 1;
inline constexpr u16 kDomainCatalogHeaderBytes = 80;
inline constexpr u16 kDomainCatalogRecordHeaderBytes = 88;
inline constexpr u16 kDomainCatalogDigestBytes = core_hash::kSha256DigestBytes;
inline constexpr u32 kDomainStringFieldCount = 25;
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
      record.domain_uuid,
      record.catalog_row_uuid,
      record.schema_uuid,
      record.default_name,
      record.base_descriptor_uuid,
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
  record->domain_uuid = fields[index++];
  record->catalog_row_uuid = fields[index++];
  record->schema_uuid = fields[index++];
  record->default_name = fields[index++];
  record->base_descriptor_uuid = fields[index++];
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

bool HasTraceTag(const EngineRequestContext& context, const std::string& tag) {
  for (const auto& candidate : context.trace_tags) {
    if (candidate == tag) { return true; }
  }
  return false;
}

bool HasDomainRight(const EngineRequestContext& context,
                    const std::string& domain_uuid,
                    const std::string& right) {
  if (!context.security_context_present) { return false; }
  if (HasTraceTag(context, "group:ROOT") || HasTraceTag(context, "role:ROOT")) { return true; }
  if (HasTraceTag(context, "right:" + right)) { return true; }
  if (!domain_uuid.empty() && HasTraceTag(context, "right:" + right + ":" + domain_uuid)) { return true; }
  if (!domain_uuid.empty() && HasTraceTag(context, "domain_right:" + domain_uuid + ":" + right)) { return true; }
  return false;
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

std::string FieldValue(const std::vector<std::pair<std::string, std::string>>& values, const std::string& field) {
  for (const auto& [name, value] : values) {
    if (name == field) { return value; }
  }
  return {};
}

bool HasField(const std::vector<std::pair<std::string, std::string>>& values, const std::string& field) {
  for (const auto& [name, ignored] : values) {
    if (name == field) { return true; }
  }
  return false;
}

void UpsertField(std::vector<std::pair<std::string, std::string>>* values,
                 const std::string& field,
                 const std::string& value) {
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

bool NumberCompare(const std::string& left, const std::string& op, const std::string& right) {
  dt::DatatypeComparisonRequest request;
  request.left = {dt::CanonicalTypeId::decimal, left, false};
  request.right = {dt::CanonicalTypeId::decimal, right, false};
  const auto compared = dt::CompareDatatypeValues(request);
  if (!compared.ok()) { return false; }
  if (op == "gt") { return compared.comparison > 0; }
  if (op == "gte") { return compared.comparison >= 0; }
  if (op == "lt") { return compared.comparison < 0; }
  if (op == "lte") { return compared.comparison <= 0; }
  if (op == "eq") { return compared.comparison == 0; }
  return false;
}

bool ParseFullNumberLiteral(const std::string& value) {
  dt::DatatypeComparisonRequest request;
  request.left = {dt::CanonicalTypeId::decimal, value, false};
  request.right = {dt::CanonicalTypeId::decimal, value, false};
  return dt::CompareDatatypeValues(request).ok();
}

bool ParseFullLengthLiteral(const std::string& value, std::size_t* out) {
  if (value.empty()) { return false; }
  try {
    std::size_t parsed = 0;
    const auto length = static_cast<std::size_t>(std::stoull(value, &parsed));
    if (parsed != value.size()) { return false; }
    if (out != nullptr) { *out = length; }
    return true;
  } catch (...) {
    return false;
  }
}

bool IsNumericDomainCheckOp(const std::string& op) {
  return op == "gt" || op == "gte" || op == "lt" || op == "lte" || op == "eq";
}

bool IsLengthDomainCheckOp(const std::string& op) {
  return op == "length_gt" || op == "length_gte" || op == "length_lt" || op == "length_lte";
}

bool IsSupportedDomainCheckPredicate(const std::string& predicate) {
  if (predicate == "not_empty") { return true; }
  if (StartsWith(predicate, "all:")) {
    const auto parts = Split(predicate.substr(4), ';');
    if (parts.empty()) { return false; }
    for (const auto& part : parts) {
      if (part.empty() || !IsSupportedDomainCheckPredicate(part)) { return false; }
    }
    return true;
  }
  const auto pos = predicate.find(':');
  if (pos == std::string::npos) { return false; }
  const std::string op = predicate.substr(0, pos);
  const std::string rhs = predicate.substr(pos + 1);
  if (IsNumericDomainCheckOp(op)) { return ParseFullNumberLiteral(rhs); }
  if (IsLengthDomainCheckOp(op)) { return ParseFullLengthLiteral(rhs, nullptr); }
  return false;
}

bool EvaluateDomainCheckPredicate(const std::string& predicate,
                                  const std::string& value,
                                  std::string* rejection_detail) {
  if (predicate == "not_empty") {
    if (!value.empty()) { return true; }
    *rejection_detail = "domain_check_not_empty_failed";
    return false;
  }
  if (StartsWith(predicate, "all:")) {
    const auto parts = Split(predicate.substr(4), ';');
    if (parts.empty()) {
      *rejection_detail = "domain_check_constraint_requires_executor_expression_support";
      return false;
    }
    for (const auto& part : parts) {
      if (part.empty()) {
        *rejection_detail = "domain_check_constraint_requires_executor_expression_support";
        return false;
      }
      if (!EvaluateDomainCheckPredicate(part, value, rejection_detail)) { return false; }
    }
    return true;
  }
  const auto pos = predicate.find(':');
  if (pos == std::string::npos) {
    *rejection_detail = "domain_check_constraint_requires_executor_expression_support";
    return false;
  }
  const std::string op = predicate.substr(0, pos);
  const std::string rhs = predicate.substr(pos + 1);
  if (IsNumericDomainCheckOp(op)) {
    if (!ParseFullNumberLiteral(rhs)) {
      *rejection_detail = "domain_check_numeric_rhs_invalid";
      return false;
    }
    if (NumberCompare(value, op, rhs)) { return true; }
    *rejection_detail = "domain_check_" + op + "_failed";
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
    *rejection_detail = "domain_check_" + op + "_failed";
    return false;
  }
  *rejection_detail = "domain_check_constraint_requires_executor_expression_support";
  return false;
}

bool CheckConstraintPasses(const std::string& envelope, const std::string& value, std::string* rejection_detail) {
  if (envelope.empty()) { return true; }
  if (StartsWith(envelope, "sblr_predicate:")) {
    const std::string predicate = envelope.substr(15);
    if (!IsSupportedDomainCheckPredicate(predicate)) {
      *rejection_detail = "domain_sblr_predicate_unsupported";
      return false;
    }
    std::string inner_detail;
    if (EvaluateDomainCheckPredicate(predicate, value, &inner_detail)) { return true; }
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
  return EvaluateDomainCheckPredicate(envelope, value, rejection_detail);
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
    const std::string required = domain.visibility_policy_envelope.substr(18);
    if (!required.empty() && context.principal_uuid.canonical == required) { return true; }
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
  std::string value;
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
    result.value = ApplyPrimitiveMask(policy, value);
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

std::string MaterializeDefault(const std::string& envelope) {
  if (StartsWith(envelope, "literal:")) { return envelope.substr(8); }
  if (StartsWith(envelope, "value:")) { return envelope.substr(6); }
  return {};
}

std::string MakeDomainEvent(const char* action, const DomainRecord& record) {
  return std::string(kDomainFoundationEventMagic) + "\t" + action + "\t" + std::to_string(record.creator_tx) + "\t" +
         record.domain_uuid + "\t" + record.catalog_row_uuid + "\t" + record.schema_uuid + "\t" +
         EncodeCrudText(record.default_name) + "\t" + record.base_descriptor_uuid + "\t" + record.base_descriptor_kind +
         "\t" + record.base_canonical_type_name + "\t" + EncodeCrudText(record.base_encoded_descriptor) + "\t" +
         (record.nullable ? "1" : "0") + "\t" + EncodeCrudText(record.default_expression_envelope) + "\t" +
         EncodeCrudText(record.check_constraint_envelope) + "\t" + EncodeCrudText(record.charset_or_collation_ref) + "\t" +
         EncodeCrudText(record.numeric_metadata) + "\t" + record.validation_hook_status + "\t" +
         EncodeCrudText(record.cast_policy_envelope) + "\t" +
         EncodeCrudText(record.mutation_policy_envelope) + "\t" +
         EncodeCrudText(record.masking_policy_envelope) + "\t" +
         EncodeCrudText(record.visibility_policy_envelope) + "\t" +
         EncodeCrudText(record.encryption_policy_ref) + "\t" +
         EncodeCrudText(record.driver_metadata_envelope) + "\t" +
         EncodeCrudText(record.wire_metadata_envelope) + "\t" +
         EncodeCrudText(record.element_path_envelope) + "\t" +
         EncodeCrudText(record.method_binding_envelope) + "\t" +
         EncodeCrudText(record.localized_names_envelope) + "\t" +
         EncodeCrudText(record.comment_envelope) + "\t" +
         EncodeCrudText(record.reference_alias_envelope);
}

std::optional<DomainBinaryAction> DomainBinaryActionFromText(const std::string& action) {
  if (action == "DOMAIN_CREATE") { return DomainBinaryAction::create; }
  if (action == "DOMAIN_ALTER") { return DomainBinaryAction::alter; }
  if (action == "DOMAIN_DROP") { return DomainBinaryAction::drop; }
  return std::nullopt;
}

bool ParseTextDomainEvent(const std::string& line,
                          DomainBinaryAction* action,
                          DomainRecord* record) {
  const auto parts = Split(line, '\t');
  if (parts.size() < 4 || parts[0] != kDomainFoundationEventMagic) { return false; }
  const auto parsed_action = DomainBinaryActionFromText(parts[1]);
  if (!parsed_action) { return false; }
  *action = *parsed_action;
  record->creator_tx = ParseU64(parts[2]);
  record->domain_uuid = parts[3];
  record->dropped = *action == DomainBinaryAction::drop;
  if (*action == DomainBinaryAction::drop) { return true; }
  if (parts.size() < 17) { return false; }
  record->catalog_row_uuid = parts[4];
  record->schema_uuid = parts[5];
  record->default_name = HexDecode(parts[6]);
  record->base_descriptor_uuid = parts[7];
  record->base_descriptor_kind = parts[8];
  record->base_canonical_type_name = parts[9];
  record->base_encoded_descriptor = HexDecode(parts[10]);
  record->nullable = ParseBool(parts[11]);
  record->default_expression_envelope = HexDecode(parts[12]);
  record->check_constraint_envelope = HexDecode(parts[13]);
  record->charset_or_collation_ref = HexDecode(parts[14]);
  record->numeric_metadata = HexDecode(parts[15]);
  record->validation_hook_status = parts[16];
  if (parts.size() > 17) { record->cast_policy_envelope = HexDecode(parts[17]); }
  if (parts.size() > 18) { record->mutation_policy_envelope = HexDecode(parts[18]); }
  if (parts.size() > 19) { record->masking_policy_envelope = HexDecode(parts[19]); }
  if (parts.size() > 20) { record->visibility_policy_envelope = HexDecode(parts[20]); }
  if (parts.size() > 21) { record->encryption_policy_ref = HexDecode(parts[21]); }
  if (parts.size() > 22) { record->driver_metadata_envelope = HexDecode(parts[22]); }
  if (parts.size() > 23) { record->wire_metadata_envelope = HexDecode(parts[23]); }
  if (parts.size() > 24) { record->element_path_envelope = HexDecode(parts[24]); }
  if (parts.size() > 25) { record->method_binding_envelope = HexDecode(parts[25]); }
  if (parts.size() > 26) { record->localized_names_envelope = HexDecode(parts[26]); }
  if (parts.size() > 27) { record->comment_envelope = HexDecode(parts[27]); }
  if (parts.size() > 28) { record->reference_alias_envelope = HexDecode(parts[28]); }
  return true;
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
  for (const auto& field : DomainStringFields(record)) {
    if (!AppendLengthPrefixedString(payload, field)) { return false; }
  }
  return true;
}

bool DeserializeDomainPayload(const std::vector<byte>& payload, DomainRecord* record) {
  std::vector<std::string> fields;
  fields.reserve(kDomainStringFieldCount);
  std::size_t offset = 0;
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

BinaryCatalogLoadResult LoadLegacyDomainTextEvents(const EngineRequestContext& context) {
  BinaryCatalogLoadResult result;
  result.ok = true;
  result.present = false;
  std::ifstream in(DomainEventPath(context), std::ios::binary);
  if (!in) { in.open(context.database_path, std::ios::binary); }
  if (!in) { return result; }
  std::string line;
  u64 sequence = 0;
  while (std::getline(in, line)) {
    if (line.rfind(kDomainFoundationEventMagic, 0) != 0) { continue; }
    BinaryDomainRecord record;
    if (!ParseTextDomainEvent(line, &record.action, &record.record)) { continue; }
    record.sequence = ++sequence;
    result.records.push_back(std::move(record));
    result.present = true;
  }
  return result;
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
  const auto parts = Split(event, '\t');
  if (parts.size() < 3 || parts[0] != kDomainFoundationEventMagic) {
    return MakeInvalidRequestDiagnostic("domain.append_event", "domain_event_invalid");
  }
  const std::uint64_t creator_tx = ParseU64(parts[2]);
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
    loaded = LoadLegacyDomainTextEvents(context);
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
  const auto path_status = ValidateCrudDatabasePath(context, "domain.append_event");
  if (path_status.error) { return path_status; }
  const auto authority_status = ValidateDomainMutatingTransactionAuthority(context, event);
  if (authority_status.error) { return authority_status; }
  BinaryDomainRecord new_record;
  if (!ParseTextDomainEvent(event, &new_record.action, &new_record.record)) {
    return MakeInvalidRequestDiagnostic("domain.append_event", "domain_event_invalid");
  }
  if (new_record.record.creator_tx != context.local_transaction_id) {
    return MakeInvalidRequestDiagnostic("domain.append_event", "domain_creator_tx_mismatch");
  }

  auto loaded = LoadBinaryDomainCatalog(context);
  if (!loaded.ok) { return loaded.diagnostic; }
  if (!loaded.present) {
    loaded = LoadLegacyDomainTextEvents(context);
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

std::string MakeDomainDropEvent(std::uint64_t creator_tx, const std::string& domain_uuid) {
  return std::string(kDomainFoundationEventMagic) + "\tDOMAIN_DROP\t" + std::to_string(creator_tx) + "\t" + domain_uuid;
}

std::optional<DomainRecord> FindVisibleDomain(const EngineRequestContext& context,
                                              const std::string& domain_uuid,
                                              std::uint64_t observer_tx) {
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

EngineDescriptor DomainDescriptor(const DomainRecord& record) {
  EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = record.domain_uuid;
  descriptor.descriptor_kind = "domain";
  descriptor.canonical_type_name = record.default_name.empty() ? record.base_canonical_type_name : record.default_name;
  descriptor.encoded_descriptor = std::string("domain_uuid=") + record.domain_uuid + ";base_descriptor_uuid=" +
                                  record.base_descriptor_uuid + ";base_type=" + record.base_canonical_type_name +
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

std::string DomainUuidFromDescriptor(const EngineDescriptor& descriptor) {
  if (descriptor.descriptor_kind == "domain" && !descriptor.descriptor_uuid.canonical.empty()) {
    return descriptor.descriptor_uuid.canonical;
  }
  return DomainUuidFromColumnDescriptor(descriptor.encoded_descriptor);
}

std::string DomainUuidFromColumnDescriptor(const std::string& column_descriptor) {
  if (StartsWith(column_descriptor, "domain:")) { return column_descriptor.substr(7); }
  return DescriptorField(column_descriptor, "domain_uuid");
}

bool IsSupportedDomainCheckEnvelope(const std::string& envelope) {
  if (envelope.empty()) { return true; }
  if (StartsWith(envelope, "sblr_predicate:")) {
    return IsSupportedDomainCheckPredicate(envelope.substr(15));
  }
  return IsSupportedDomainCheckPredicate(envelope);
}

bool DomainChainContainsUuid(const EngineRequestContext& context,
                             const std::string& start_domain_uuid,
                             const std::string& searched_domain_uuid,
                             std::uint64_t observer_tx) {
  if (start_domain_uuid.empty() || searched_domain_uuid.empty()) { return false; }
  std::string current = start_domain_uuid;
  std::set<std::string> seen;
  for (std::uint32_t depth = 0; depth < 32 && !current.empty(); ++depth) {
    if (current == searched_domain_uuid) { return true; }
    if (!seen.insert(current).second) { return false; }
    const auto domain = FindVisibleDomain(context, current, observer_tx);
    if (!domain) { return false; }
    std::string next;
    if (domain->base_descriptor_kind == "domain" && !domain->base_descriptor_uuid.empty()) {
      next = domain->base_descriptor_uuid;
    } else {
      next = DomainUuidFromColumnDescriptor(domain->base_encoded_descriptor);
    }
    current = next;
  }
  return false;
}

DomainValueValidationResult ValidateDomainTypedValue(const EngineRequestContext& context,
                                                     const EngineDescriptor& domain_descriptor,
                                                     const EngineTypedValue& input_value,
                                                     std::uint64_t observer_tx) {
  DomainValueValidationResult result;
  const std::string domain_uuid = DomainUuidFromDescriptor(domain_descriptor);
  if (domain_uuid.empty()) {
    result.diagnostic = DomainValidationDiagnostic("domain_uuid_required");
    return result;
  }
  const auto domain = FindVisibleDomain(context, domain_uuid, observer_tx);
  if (!domain) {
    result.diagnostic = DomainValidationDiagnostic("domain_not_visible");
    return result;
  }
  const std::string cast_right = RequiredRightFromPolicy(domain->cast_policy_envelope);
  if (!cast_right.empty() && !HasDomainRight(context, domain_uuid, cast_right)) {
    result.diagnostic = DomainValidationDiagnostic("domain_cast_right_denied:" + cast_right);
    return result;
  }
  if (input_value.is_null || input_value.encoded_value == "<NULL>") {
    if (!domain->nullable) {
      result.diagnostic = DomainValidationDiagnostic("domain_null_forbidden");
      return result;
    }
    result.ok = true;
    result.value = input_value;
    result.value.descriptor = DomainDescriptor(*domain);
    result.value.is_null = true;
    result.evidence.push_back({"domain_validation", domain_uuid});
    return result;
  }
  EngineTypedValue value_for_base_cast = input_value;
  if (domain->base_descriptor_kind == "domain" && !domain->base_descriptor_uuid.empty()) {
    if (domain->base_descriptor_uuid == domain_uuid ||
        DomainChainContainsUuid(context, domain->base_descriptor_uuid, domain_uuid, observer_tx)) {
      result.diagnostic = DomainValidationDiagnostic("domain_chain_cycle_detected");
      return result;
    }
    const auto base_domain = FindVisibleDomain(context, domain->base_descriptor_uuid, observer_tx);
    if (!base_domain) {
      result.diagnostic = DomainValidationDiagnostic("domain_base_not_visible");
      return result;
    }
    const auto base_validation = ValidateDomainTypedValue(context,
                                                          DomainDescriptor(*base_domain),
                                                          input_value,
                                                          observer_tx);
    if (!base_validation.ok) {
      result.diagnostic = base_validation.diagnostic;
      return result;
    }
    value_for_base_cast = base_validation.value;
    result.evidence.push_back({"domain_base_validation", domain->base_descriptor_uuid});
    for (const auto& evidence : base_validation.evidence) { result.evidence.push_back(evidence); }
  }
  const auto target_type = dt::CanonicalTypeIdFromStableName(domain->base_canonical_type_name);
  const auto source_type = value_for_base_cast.descriptor.canonical_type_name.empty()
                               ? dt::CanonicalTypeId::character
                               : dt::CanonicalTypeIdFromStableName(value_for_base_cast.descriptor.canonical_type_name);
  dt::DatatypeCastRequest cast_request;
  cast_request.value.type_id = source_type == dt::CanonicalTypeId::unknown ? dt::CanonicalTypeId::character : source_type;
  cast_request.value.encoded_value = value_for_base_cast.encoded_value;
  cast_request.value.is_null = false;
  cast_request.target_type_id = target_type == dt::CanonicalTypeId::unknown ? dt::CanonicalTypeId::character : target_type;
  cast_request.explicit_cast = true;
  const auto cast = dt::CastDatatypeValue(cast_request);
  if (!cast.ok()) {
    result.diagnostic = DomainValidationDiagnostic("domain_base_cast_failed");
    return result;
  }
  std::string check_detail;
  if (!CheckConstraintPasses(domain->check_constraint_envelope, cast.value.encoded_value, &check_detail)) {
    result.diagnostic = DomainValidationDiagnostic(check_detail);
    return result;
  }
  result.ok = true;
  result.value = input_value;
  result.value.descriptor = DomainDescriptor(*domain);
  result.value.encoded_value = cast.value.encoded_value;
  result.value.is_null = false;
  result.evidence.push_back({"domain_validation", domain_uuid});
  if (!domain->check_constraint_envelope.empty()) { result.evidence.push_back({"domain_check", domain_uuid}); }
  return result;
}

DomainRowValidationResult ApplyDomainRulesToCrudValues(
    const EngineRequestContext& context,
    const std::vector<std::pair<std::string, std::string>>& table_columns,
    const std::vector<std::pair<std::string, std::string>>& input_values,
    std::uint64_t observer_tx,
    ConstraintDmlValidationCache* cache) {
  DomainRowValidationResult result;
  result.values = input_values;
  for (const auto& [column_name, column_descriptor] : table_columns) {
    const std::string domain_uuid = DomainUuidFromColumnDescriptor(column_descriptor);
    if (domain_uuid.empty()) { continue; }
    const auto domain = FindVisibleDomain(context, domain_uuid, observer_tx);
    if (!domain) {
      result.diagnostic = DomainValidationDiagnostic("domain_not_visible_for_column:" + column_name);
      return result;
    }
    const std::string use_right = RequiredRightFromPolicy(domain->mutation_policy_envelope);
    if (!use_right.empty() && !HasDomainRight(context, domain_uuid, use_right)) {
      result.diagnostic = DomainValidationDiagnostic("domain_use_right_denied:" + column_name + ":" + use_right);
      return result;
    }
    const bool present = HasField(result.values, column_name);
    if (!present) {
      const std::string default_value = MaterializeDefault(domain->default_expression_envelope);
      if (!default_value.empty()) {
        UpsertField(&result.values, column_name, default_value);
      } else if (!domain->nullable) {
        result.diagnostic = DomainValidationDiagnostic("domain_required_column_missing:" + column_name);
        return result;
      } else {
        continue;
      }
    }
    EngineDescriptor descriptor = DomainDescriptor(*domain);
    EngineTypedValue value;
    value.descriptor.canonical_type_name = "character";
    value.encoded_value = FieldValue(result.values, column_name);
    value.is_null = value.encoded_value == "<NULL>";
    const std::string proof_identity =
        domain_uuid + "\n" + column_name + "\n" +
        domain->base_descriptor_uuid + "\n" + domain->base_descriptor_kind + "\n" +
        domain->base_canonical_type_name + "\n" + domain->check_constraint_envelope + "\n" +
        (domain->nullable ? "nullable" : "not_nullable") + "\n" +
        std::to_string(observer_tx) + "\n" + value.encoded_value;
    if (const auto cached_value = FindConstraintDmlProofPayload(cache,
                                                                context,
                                                                "domain_check",
                                                                proof_identity,
                                                                &result.evidence)) {
      UpsertField(&result.values, column_name, *cached_value);
      result.evidence.push_back({"domain_validation", domain_uuid});
      if (!domain->check_constraint_envelope.empty()) {
        result.evidence.push_back({"domain_check", domain_uuid});
      }
      continue;
    }
    const auto validation = ValidateDomainTypedValue(context, descriptor, value, observer_tx);
    if (!validation.ok) {
      result.diagnostic = validation.diagnostic;
      return result;
    }
    UpsertField(&result.values, column_name, validation.value.is_null ? "<NULL>" : validation.value.encoded_value);
    for (const auto& evidence : validation.evidence) { result.evidence.push_back(evidence); }
    StoreConstraintDmlProof(cache,
                            context,
                            "domain_check",
                            proof_identity,
                            validation.value.is_null ? std::string("<NULL>") : validation.value.encoded_value,
                            &result.evidence);
  }
  result.ok = true;
  result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return result;
}

bool DomainHasCrudDependencies(const EngineRequestContext& context,
                               const std::string& domain_uuid,
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
    const std::vector<std::pair<std::string, std::string>>& input_values,
    std::uint64_t observer_tx) {
  DomainReadPolicyResult result;
  result.values = input_values;
  for (const auto& [column_name, column_descriptor] : table_columns) {
    const std::string domain_uuid = DomainUuidFromColumnDescriptor(column_descriptor);
    if (domain_uuid.empty()) { continue; }
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
      const std::string value = FieldValue(result.values, column_name);
      if (HasField(result.values, column_name) && value != "<NULL>") {
        const auto mask = ApplyDomainMaskPolicy(context, *domain, column_name, value);
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
