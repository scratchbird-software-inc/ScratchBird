// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbps_narrow_statement_context_alias_codec.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

namespace scratchbird::parser::ipc {
namespace {

constexpr const char* kFrameInvalid =
    "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID";
constexpr const char* kSessionMismatch =
    "PARSER_SERVER_IPC.SESSION_MISMATCH";
constexpr const char* kTransactionStale = "MGA.TRANSACTION.STALE";
constexpr const char* kResourceExceeded =
    "PARSER_SERVER_IPC.RESOURCE_LIMIT_EXCEEDED";
constexpr const char* kBudgetExceeded = "RESOURCE.BUDGET_EXCEEDED";
constexpr std::size_t kSchema7032BasePrefixBytes =
    2 + 1 + (6 * 16) + (2 * 8);
constexpr std::size_t kDescriptorProfileBytes = 64;
constexpr std::uint16_t kAggregateFunctionCountV11 = 43;
constexpr std::uint16_t kWindowFunctionCountV11 = 11;
constexpr std::uint16_t kDescriptorProfileCountV11 = 646;

PsStatementContextAliasDiagnosticV1 Ok() {
  PsStatementContextAliasDiagnosticV1 result;
  result.status = PsStatementContextAliasStatusV1::ok;
  return result;
}

PsStatementContextAliasDiagnosticV1 Error(
    PsStatementContextAliasStatusV1 status,
    std::string diagnostic_code,
    std::string field,
    std::string detail) {
  PsStatementContextAliasDiagnosticV1 result;
  result.status = status;
  result.diagnostic_code = std::move(diagnostic_code);
  result.field = std::move(field);
  result.detail = std::move(detail);
  return result;
}

PsStatementContextAliasDiagnosticV1 Invalid(std::string field,
                                            std::string detail) {
  return Error(PsStatementContextAliasStatusV1::source_schema_invalid,
               kFrameInvalid, std::move(field), std::move(detail));
}

std::uint16_t LoadU16(std::span<const byte> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         (static_cast<std::uint16_t>(bytes[offset + 1]) << 8u);
}

std::uint32_t LoadU32(std::span<const byte> bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (unsigned shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(bytes[offset + shift / 8]) << shift;
  }
  return value;
}

std::uint64_t LoadU64(std::span<const byte> bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned shift = 0; shift < 64; shift += 8) {
    value |= static_cast<std::uint64_t>(bytes[offset + shift / 8]) << shift;
  }
  return value;
}

void AppendU16(std::vector<byte>* bytes, std::uint16_t value) {
  bytes->push_back(static_cast<byte>(value & 0xffu));
  bytes->push_back(static_cast<byte>((value >> 8u) & 0xffu));
}

void AppendU64(std::vector<byte>* bytes, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    bytes->push_back(static_cast<byte>((value >> shift) & 0xffu));
  }
}

PsStatementContextUuidV1 LoadUuid(std::span<const byte> bytes,
                                  std::size_t offset) {
  PsStatementContextUuidV1 result{};
  std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
              result.size(), result.begin());
  return result;
}

bool UuidPresent(const PsStatementContextUuidV1& value) {
  return std::any_of(value.begin(), value.end(),
                     [](byte octet) { return octet != 0; });
}

bool UuidV7(const PsStatementContextUuidV1& value) {
  return UuidPresent(value) && (value[6] & 0xf0u) == 0x70u &&
         (value[8] & 0xc0u) == 0x80u;
}

bool AllZero(std::span<const byte> value) {
  return std::all_of(value.begin(), value.end(),
                     [](byte octet) { return octet == 0; });
}

bool AnyNonzero(std::span<const byte> value) {
  return std::any_of(value.begin(), value.end(),
                     [](byte octet) { return octet != 0; });
}

bool ExactPair(const PsStatementContextUuidV1& uuid,
               std::uint64_t generation) {
  return UuidPresent(uuid) == (generation != 0);
}

bool AddWithin(std::size_t left,
               std::size_t right,
               std::size_t maximum,
               std::size_t* result) {
  if (result == nullptr || right > maximum || left > maximum - right) {
    return false;
  }
  *result = left + right;
  return true;
}

bool ReadU16String(std::span<const byte> bytes,
                   std::size_t* offset,
                   std::string_view* value) {
  if (offset == nullptr || value == nullptr || *offset > bytes.size() ||
      bytes.size() - *offset < 2) {
    return false;
  }
  const auto length = LoadU16(bytes, *offset);
  *offset += 2;
  if (length > bytes.size() - *offset) return false;
  *value = std::string_view(
      reinterpret_cast<const char*>(bytes.data() + *offset), length);
  *offset += length;
  return true;
}

bool ValidCanonicalUtf8(std::string_view value) {
  if (value.empty()) return false;
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto first = static_cast<unsigned char>(value[offset]);
    if (first == 0) return false;
    if (first <= 0x7fu) {
      ++offset;
      continue;
    }
    std::size_t continuation_count = 0;
    std::uint32_t code_point = 0;
    if (first >= 0xc2u && first <= 0xdfu) {
      continuation_count = 1;
      code_point = first & 0x1fu;
    } else if (first >= 0xe0u && first <= 0xefu) {
      continuation_count = 2;
      code_point = first & 0x0fu;
    } else if (first >= 0xf0u && first <= 0xf4u) {
      continuation_count = 3;
      code_point = first & 0x07u;
    } else {
      return false;
    }
    if (continuation_count > value.size() - offset - 1) return false;
    for (std::size_t index = 0; index < continuation_count; ++index) {
      const auto next =
          static_cast<unsigned char>(value[offset + index + 1]);
      if ((next & 0xc0u) != 0x80u) return false;
      code_point = (code_point << 6u) | (next & 0x3fu);
    }
    if ((continuation_count == 2 && code_point < 0x800u) ||
        (continuation_count == 3 && code_point < 0x10000u) ||
        (code_point >= 0xd800u && code_point <= 0xdfffu) ||
        code_point > 0x10ffffu) {
      return false;
    }
    offset += continuation_count + 1;
  }
  return true;
}

bool ValidStatementTimestamp(std::string_view value) {
  if (value.size() != 20 && (value.size() < 22 || value.size() > 30)) {
    return false;
  }
  if (value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':' || value.back() != 'Z') {
    return false;
  }
  constexpr std::size_t kDigits[] = {
      0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
  for (const auto index : kDigits) {
    if (value[index] < '0' || value[index] > '9') return false;
  }
  if (value.size() > 20) {
    if (value[19] != '.') return false;
    for (std::size_t index = 20; index + 1 < value.size(); ++index) {
      if (value[index] < '0' || value[index] > '9') return false;
    }
  }
  const auto decimal = [&](std::size_t offset, std::size_t digits) {
    unsigned result = 0;
    for (std::size_t index = 0; index < digits; ++index) {
      result = result * 10 +
               static_cast<unsigned>(value[offset + index] - '0');
    }
    return result;
  };
  const auto year = decimal(0, 4);
  const auto month = decimal(5, 2);
  const auto day = decimal(8, 2);
  const auto hour = decimal(11, 2);
  const auto minute = decimal(14, 2);
  const auto second = decimal(17, 2);
  if (year == 0 || month == 0 || month > 12 || hour > 23 || minute > 59 ||
      second > 59) {
    return false;
  }
  constexpr unsigned kDaysByMonth[] = {
      0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  auto maximum_day = kDaysByMonth[month];
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (month == 2 && leap) ++maximum_day;
  return day != 0 && day <= maximum_day;
}

PsStatementContextAliasDiagnosticV1 ValidateRequestContext(
    const PsNarrowStatementContextRequestValidationContextV1& context) {
  if (!UuidPresent(context.expected_session_uuid) ||
      context.expected_owning_local_transaction_id == 0 ||
      !UuidPresent(context.expected_owning_transaction_uuid)) {
    return Error(PsStatementContextAliasStatusV1::invalid_argument,
                 kFrameInvalid, "validation_context",
                 "statement_context_request_authority_is_incomplete");
  }
  return Ok();
}

PsStatementContextAliasDiagnosticV1 ValidateRequest(
    const PsNarrowStatementContextRequestV1& request,
    const PsNarrowStatementContextRequestValidationContextV1& context) {
  auto outcome = ValidateRequestContext(context);
  if (!outcome.ok()) return outcome;
  if (!UuidPresent(request.session_uuid) ||
      request.session_uuid != context.expected_session_uuid) {
    return Error(PsStatementContextAliasStatusV1::request_authority_mismatch,
                 kSessionMismatch, "session_uuid",
                 "schema7031_session_does_not_match_bound_frame");
  }
  if (request.owning_local_transaction_id == 0 ||
      !UuidPresent(request.owning_transaction_uuid) ||
      request.owning_local_transaction_id !=
          context.expected_owning_local_transaction_id ||
      request.owning_transaction_uuid !=
          context.expected_owning_transaction_uuid) {
    return Error(PsStatementContextAliasStatusV1::request_authority_mismatch,
                 kTransactionStale, "owning_transaction",
                 "schema7031_transaction_selector_is_not_live_authority");
  }
  return Ok();
}

PsStatementContextAliasDiagnosticV1 ValidateFunctionProfiles(
    std::span<const byte> payload,
    std::size_t* offset,
    std::uint16_t expected_count,
    std::string_view required_prefix,
    std::string_view field) {
  if (offset == nullptr || *offset > payload.size() ||
      payload.size() - *offset < 2 ||
      LoadU16(payload, *offset) != expected_count) {
    return Invalid(std::string(field), "function_profile_count_invalid");
  }
  *offset += 2;
  std::set<std::string_view> builtin_ids;
  std::set<PsStatementContextUuidV1> function_uuids;
  for (std::uint16_t index = 0; index < expected_count; ++index) {
    if (*offset > payload.size() || payload.size() - *offset < 2 ||
        LoadU16(payload, *offset) != 1) {
      return Invalid(std::string(field),
                     "function_profile_abi_version_invalid");
    }
    *offset += 2;
    std::string_view builtin_id;
    if (!ReadU16String(payload, offset, &builtin_id) ||
        builtin_id.size() <= required_prefix.size() ||
        !builtin_id.starts_with(required_prefix) ||
        builtin_id.size() > 4096 || !ValidCanonicalUtf8(builtin_id) ||
        *offset > payload.size() || payload.size() - *offset < 17) {
      return Invalid(std::string(field), "function_profile_name_invalid");
    }
    const auto uuid = LoadUuid(payload, *offset);
    *offset += 16;
    const auto executable = payload[(*offset)++];
    if (!UuidPresent(uuid) || executable != 1 ||
        !builtin_ids.insert(builtin_id).second ||
        !function_uuids.insert(uuid).second) {
      return Invalid(std::string(field),
                     "function_profile_identity_or_executable_invalid");
    }
  }
  return Ok();
}

PsStatementContextAliasDiagnosticV1 LocateAndValidateSchema7032Base(
    std::span<const byte> payload,
    std::size_t* extension_offset,
    PsStatementContextUuidV1* owning_transaction_uuid,
    std::uint64_t* owning_local_transaction_id,
    PsStatementContextUuidV1* statement_snapshot_uuid) {
  if (extension_offset == nullptr || owning_transaction_uuid == nullptr ||
      owning_local_transaction_id == nullptr ||
      statement_snapshot_uuid == nullptr ||
      payload.size() < kSchema7032BasePrefixBytes ||
      LoadU16(payload, 0) != kPsStatementContextProjectionVersionV11 ||
      payload[2] != 1) {
    return Invalid("schema7032_base", "projection_version_or_outcome_invalid");
  }
  const auto statement_uuid = LoadUuid(payload, 3);
  const auto local_transaction_id = LoadU64(payload, 19);
  const auto transaction_uuid = LoadUuid(payload, 27);
  const auto statement_snapshot = LoadUuid(payload, 43);
  const auto statement_metadata_snapshot = LoadUuid(payload, 59);
  const auto catalog_epoch = LoadUuid(payload, 75);
  const auto security_context = LoadUuid(payload, 91);
  if (!UuidPresent(statement_uuid) || local_transaction_id == 0 ||
      !UuidPresent(transaction_uuid) || !UuidPresent(statement_snapshot) ||
      !UuidPresent(statement_metadata_snapshot) ||
      !UuidPresent(catalog_epoch) || !UuidPresent(security_context)) {
    return Invalid("schema7032_base", "base_identity_is_zero");
  }

  std::size_t offset = kSchema7032BasePrefixBytes;
  std::string_view timestamp;
  if (!ReadU16String(payload, &offset, &timestamp) ||
      !ValidStatementTimestamp(timestamp)) {
    return Invalid("statement_timestamp", "timestamp_is_not_canonical_utc");
  }
  if (offset > payload.size() || payload.size() - offset < 6 * 16) {
    return Invalid("schema7032_base", "callable_identity_prefix_truncated");
  }
  for (std::size_t index = 0; index < 6; ++index) {
    if (!UuidPresent(LoadUuid(payload, offset + index * 16))) {
      return Invalid("schema7032_base", "callable_identity_is_zero");
    }
  }
  offset += 6 * 16;
  auto outcome = ValidateFunctionProfiles(
      payload, &offset, kAggregateFunctionCountV11, "sb.aggregate.",
      "aggregate_function_profiles");
  if (!outcome.ok()) return outcome;
  outcome = ValidateFunctionProfiles(payload, &offset,
                                     kWindowFunctionCountV11, "sb.window.",
                                     "window_function_profiles");
  if (!outcome.ok()) return outcome;
  if (offset > payload.size() || payload.size() - offset < 2 ||
      LoadU16(payload, offset) != kDescriptorProfileCountV11) {
    return Invalid("descriptor_profiles", "profile_count_invalid");
  }
  offset += 2;
  std::size_t profiles_end = 0;
  if (!AddWithin(offset,
                 static_cast<std::size_t>(kDescriptorProfileCountV11) *
                     kDescriptorProfileBytes,
                 payload.size(), &profiles_end)) {
    return Invalid("descriptor_profiles", "profile_vector_truncated");
  }

  std::set<PsStatementContextUuidV1> descriptor_uuids;
  std::array<PsStatementContextUuidV1, 24> exact_type_uuids{};
  for (std::uint16_t index = 0; index < kDescriptorProfileCountV11; ++index) {
    const auto at = offset + static_cast<std::size_t>(index) *
                                 kDescriptorProfileBytes;
    std::uint8_t expected_kind = 0;
    std::uint16_t expected_slot = 0;
    if (index < 320) {
      expected_kind = static_cast<std::uint8_t>(index / 32 + 1);
      expected_slot = static_cast<std::uint16_t>(index % 32);
    } else if (index < 322) {
      expected_kind = 11;
      expected_slot = static_cast<std::uint16_t>(index - 320);
    } else if (index < 324) {
      expected_kind = 12;
      expected_slot = static_cast<std::uint16_t>(index - 322);
    } else if (index < 326) {
      expected_kind = 13;
      expected_slot = static_cast<std::uint16_t>(index - 324);
    } else {
      expected_kind = static_cast<std::uint8_t>(14 + (index - 326) / 32);
      expected_slot = static_cast<std::uint16_t>((index - 326) % 32);
    }
    const auto kind = payload[at];
    const auto slot = LoadU16(payload, at + 1);
    const auto descriptor_uuid = LoadUuid(payload, at + 3);
    const auto type_uuid = LoadUuid(payload, at + 19);
    const auto collation_uuid = LoadUuid(payload, at + 35);
    const auto nullable = payload[at + 51];
    const auto width = LoadU32(payload, at + 52);
    const auto precision = LoadU32(payload, at + 56);
    const auto scale = LoadU32(payload, at + 60);
    const bool expected_nullable =
        (kind <= 10 && kind % 2 == 0) || (kind >= 14 && kind % 2 == 1);
    if (kind != expected_kind || slot != expected_slot ||
        !UuidV7(descriptor_uuid) || !UuidPresent(type_uuid) ||
        (type_uuid[6] & 0xf0u) == 0 ||
        (type_uuid[8] & 0xc0u) != 0x80u || nullable > 1 ||
        (nullable == 1) != expected_nullable || scale > precision ||
        !descriptor_uuids.insert(descriptor_uuid).second ||
        (kind >= 11 &&
         (UuidPresent(collation_uuid) || width != 0 || precision != 0 ||
          scale != 0))) {
      return Invalid("descriptor_profiles",
                     "exact_profile_cohort_or_identity_invalid");
    }
    if (kind >= 11) {
      auto& retained_type = exact_type_uuids[kind];
      if (!UuidPresent(retained_type)) {
        retained_type = type_uuid;
      } else if (retained_type != type_uuid) {
        return Invalid("descriptor_profiles",
                       "profile_kind_type_identity_drifted");
      }
    }
  }
  if (!UuidPresent(exact_type_uuids[11]) ||
      !UuidPresent(exact_type_uuids[12]) ||
      !UuidPresent(exact_type_uuids[13]) ||
      exact_type_uuids[11] == exact_type_uuids[12] ||
      exact_type_uuids[11] == exact_type_uuids[13] ||
      exact_type_uuids[12] == exact_type_uuids[13] ||
      exact_type_uuids[14] != exact_type_uuids[15] ||
      exact_type_uuids[16] != exact_type_uuids[17] ||
      exact_type_uuids[18] != exact_type_uuids[19] ||
      exact_type_uuids[20] != exact_type_uuids[21] ||
      exact_type_uuids[22] != exact_type_uuids[23] ||
      exact_type_uuids[14] != exact_type_uuids[12] ||
      exact_type_uuids[16] != exact_type_uuids[13] ||
      exact_type_uuids[18] != exact_type_uuids[11]) {
    return Invalid("descriptor_profiles", "multileg_type_cohort_invalid");
  }
  const std::array<PsStatementContextUuidV1, 5> multileg_types{
      exact_type_uuids[14], exact_type_uuids[16], exact_type_uuids[18],
      exact_type_uuids[20], exact_type_uuids[22]};
  if (std::set<PsStatementContextUuidV1>(multileg_types.begin(),
                                        multileg_types.end())
          .size() != multileg_types.size()) {
    return Invalid("descriptor_profiles",
                   "multileg_type_identities_are_not_distinct");
  }

  *extension_offset = profiles_end;
  *owning_transaction_uuid = transaction_uuid;
  *owning_local_transaction_id = local_transaction_id;
  *statement_snapshot_uuid = statement_snapshot;
  return Ok();
}

PsStatementContextAliasDiagnosticV1 ValidateSchema7032ExtensionV71(
    std::span<const byte> payload,
    std::size_t extension,
    const PsStatementContextUuidV1& owning_transaction_uuid,
    std::uint64_t owning_local_transaction_id,
    const PsStatementContextUuidV1& statement_snapshot_uuid,
    PsNarrowStatementContextResultSummaryV1* summary) {
  if (summary == nullptr || extension > payload.size() ||
      payload.size() - extension <
          kPsStatementContextExtensionPrefixBytesV71 +
              kPsStatementContextTrailerBytesV71 ||
      LoadU16(payload, extension) !=
          kPsStatementContextExtensionWireVersionV71 ||
      LoadU16(payload, extension + 2) != 0) {
    return Invalid("extension_wire_version",
                   "schema7032_extension_is_not_exact_v71");
  }
  const auto receipt_uuid = LoadUuid(payload, extension + 4);
  const auto literal_catalog_uuid = LoadUuid(payload, extension + 20);
  const auto literal_generation = LoadU64(payload, extension + 36);
  const auto security_epoch = LoadU64(payload, extension + 44);
  const auto resource_epoch = LoadU64(payload, extension + 52);
  const auto extension_snapshot = LoadUuid(payload, extension + 60);
  const auto prepared_uuid = LoadUuid(payload, extension + 76);
  const auto prepared_generation = LoadU64(payload, extension + 92);
  const auto batch_uuid = LoadUuid(payload, extension + 100);
  const auto batch_generation = LoadU64(payload, extension + 116);
  const auto dynamic_uuid = LoadUuid(payload, extension + 124);
  const auto dynamic_generation = LoadU64(payload, extension + 140);
  const auto parameter_executor_generation = LoadU64(payload, extension + 148);
  const auto variable_scope_uuid = LoadUuid(payload, extension + 156);
  const auto variable_scope_generation = LoadU64(payload, extension + 172);
  const auto variable_frame_uuid = LoadUuid(payload, extension + 180);
  const auto variable_frame_generation = LoadU64(payload, extension + 196);
  const auto variable_registry_uuid = LoadUuid(payload, extension + 204);
  const auto variable_executor_generation = LoadU64(payload, extension + 220);
  const auto diagnostic_registry_uuid = LoadUuid(payload, extension + 228);
  const auto diagnostic_generation = LoadU64(payload, extension + 244);
  const auto diagnostic_count = LoadU32(payload, extension + 252);
  const auto diagnostic_row_bytes = LoadU32(payload, extension + 256);
  if (!UuidPresent(receipt_uuid) || !UuidPresent(literal_catalog_uuid) ||
      literal_generation == 0 || security_epoch == 0 || resource_epoch == 0 ||
      extension_snapshot != statement_snapshot_uuid ||
      !ExactPair(prepared_uuid, prepared_generation) ||
      !ExactPair(batch_uuid, batch_generation) ||
      !ExactPair(dynamic_uuid, dynamic_generation) ||
      parameter_executor_generation == 0 ||
      !ExactPair(variable_scope_uuid, variable_scope_generation) ||
      !ExactPair(variable_frame_uuid, variable_frame_generation) ||
      !ExactPair(variable_registry_uuid, variable_executor_generation) ||
      !UuidPresent(diagnostic_registry_uuid) || diagnostic_generation == 0 ||
      diagnostic_count == 0 || diagnostic_count > 4096 ||
      diagnostic_row_bytes != kPsStatementContextDiagnosticRowBytesV71) {
    return Invalid("extension_prefix",
                   "schema7032_v71_extension_identity_matrix_invalid");
  }

  std::size_t diagnostic_bytes = 0;
  if (diagnostic_count >
      std::numeric_limits<std::size_t>::max() /
          kPsStatementContextDiagnosticRowBytesV71) {
    return Error(PsStatementContextAliasStatusV1::resource_limit_exceeded,
                 kResourceExceeded, "diagnostic_identity_rows",
                 "diagnostic_row_extent_overflow");
  }
  diagnostic_bytes = static_cast<std::size_t>(diagnostic_count) *
                     kPsStatementContextDiagnosticRowBytesV71;
  std::size_t trailer = 0;
  if (!AddWithin(extension, kPsStatementContextExtensionPrefixBytesV71,
                 payload.size(), &trailer) ||
      !AddWithin(trailer, diagnostic_bytes, payload.size(), &trailer) ||
      payload.size() - trailer != kPsStatementContextTrailerBytesV71) {
    return Invalid("schema7032_extent",
                   "v71_diagnostic_or_trailer_extent_invalid");
  }
  for (std::uint32_t index = 0; index < diagnostic_count; ++index) {
    const auto at = extension + kPsStatementContextExtensionPrefixBytesV71 +
                    static_cast<std::size_t>(index) *
                        kPsStatementContextDiagnosticRowBytesV71;
    if (!UuidPresent(LoadUuid(payload, at)) || LoadU64(payload, at + 16) == 0 ||
        LoadU16(payload, at + 30) != 0 || LoadU32(payload, at + 36) != 0) {
      return Invalid("diagnostic_identity_rows",
                     "diagnostic_identity_record_invalid");
    }
  }

  const auto isolation_uuid = LoadUuid(payload, trailer);
  const auto isolation_generation = LoadU64(payload, trailer + 16);
  const auto policy_uuid = LoadUuid(payload, trailer + 24);
  const auto policy_generation = LoadU64(payload, trailer + 40);
  const auto begin_executor_generation = LoadU64(payload, trailer + 48);
  const auto begin_read_mode = payload[trailer + 64];
  const auto begin_scope = payload[trailer + 65];
  const auto begin_wait = payload[trailer + 66];
  const auto commit_generation = LoadU64(payload, trailer + 80);
  const auto rollback_generation = LoadU64(payload, trailer + 104);
  if (!UuidPresent(isolation_uuid) || isolation_generation == 0 ||
      !UuidPresent(policy_uuid) || policy_generation == 0 ||
      begin_executor_generation == 0 || LoadU64(payload, trailer + 56) != 0 ||
      begin_read_mode < 1 || begin_read_mode > 2 || begin_scope < 1 ||
      begin_scope > 2 || begin_wait < 1 || begin_wait > 2 ||
      !AllZero(payload.subspan(trailer + 67, 5)) || commit_generation == 0 ||
      payload[trailer + 88] != 1 || payload[trailer + 89] < 1 ||
      payload[trailer + 89] > 2 || payload[trailer + 90] < 1 ||
      payload[trailer + 90] > 2 ||
      !AllZero(payload.subspan(trailer + 91, 5)) ||
      rollback_generation == 0 || payload[trailer + 112] != 1 ||
      payload[trailer + 113] < 1 || payload[trailer + 113] > 2 ||
      payload[trailer + 114] < 1 || payload[trailer + 114] > 2 ||
      !AllZero(payload.subspan(trailer + 115, 5))) {
    return Invalid("transaction_executor_tuple",
                   "transaction_begin_commit_or_rollback_tuple_invalid");
  }
  for (std::size_t index = 0; index < 61; ++index) {
    if (LoadU64(payload, trailer + 128 + index * 8) == 0) {
      return Invalid("executor_availability_generations",
                     "executor_generation_is_zero");
    }
  }

  const auto handle = trailer + 616;
  const std::array<byte, 4> txbh{'T', 'X', 'B', 'H'};
  if (!std::equal(txbh.begin(), txbh.end(), payload.begin() + handle) ||
      LoadU16(payload, handle + 4) != 1 ||
      LoadU16(payload, handle + 6) !=
          kPsStatementContextTransactionHandleBytesV1 ||
      LoadU32(payload, handle + 8) !=
          kPsStatementContextTransactionHandleBytesV1 ||
      LoadU32(payload, handle + 12) != 0 ||
      LoadUuid(payload, handle + 16) != owning_transaction_uuid ||
      LoadU64(payload, handle + 32) != owning_local_transaction_id ||
      !UuidPresent(LoadUuid(payload, handle + 40)) ||
      LoadUuid(payload, handle + 56) != isolation_uuid ||
      LoadU64(payload, handle + 72) != isolation_generation ||
      LoadUuid(payload, handle + 80) != policy_uuid ||
      LoadU64(payload, handle + 96) != policy_generation ||
      payload[handle + 104] != begin_read_mode || payload[handle + 105] != 1 ||
      payload[handle + 106] != begin_scope ||
      !AllZero(payload.subspan(handle + 107, 5)) ||
      !AnyNonzero(payload.subspan(handle + 112, 32)) ||
      LoadU64(payload, handle + 144) != begin_executor_generation) {
    return Invalid("active_transaction_handle",
                   "txbh_v1_does_not_match_statement_transaction");
  }
  const auto scan_bound = LoadU64(payload, trailer + 768);
  if (scan_bound < kPsStatementContextMinimumMgaDecodedBytesPerPass ||
      scan_bound > kPsStatementContextMaximumMgaDecodedBytesPerPass) {
    return Error(PsStatementContextAliasStatusV1::resource_limit_exceeded,
                 kBudgetExceeded,
                 "maximum_mga_relation_decoded_bytes_per_pass",
                 "scan_bound_outside_inclusive_core_range");
  }

  summary->extension_offset = extension;
  summary->diagnostic_identity_row_count = diagnostic_count;
  summary->maximum_mga_relation_decoded_bytes_per_pass = scan_bound;
  summary->preliminary_receipt_uuid = receipt_uuid;
  summary->owning_transaction_uuid = owning_transaction_uuid;
  summary->owning_local_transaction_id = owning_local_transaction_id;
  return Ok();
}

}  // namespace

PsNarrowStatementContextRequestCodecResultV1
EncodeAndValidatePsNarrowStatementContextRequestV1(
    const PsNarrowStatementContextRequestV1& request,
    const PsNarrowStatementContextRequestValidationContextV1& context) {
  PsNarrowStatementContextRequestCodecResultV1 result;
  result.outcome = ValidateRequest(request, context);
  if (!result.outcome.ok()) return result;
  result.canonical_payload.reserve(kPsStatementContextRequestBytesV11);
  AppendU16(&result.canonical_payload,
            kPsStatementContextProjectionVersionV11);
  result.canonical_payload.insert(result.canonical_payload.end(),
                                  request.session_uuid.begin(),
                                  request.session_uuid.end());
  AppendU64(&result.canonical_payload,
            request.owning_local_transaction_id);
  result.canonical_payload.insert(result.canonical_payload.end(),
                                  request.owning_transaction_uuid.begin(),
                                  request.owning_transaction_uuid.end());
  if (result.canonical_payload.size() != kPsStatementContextRequestBytesV11) {
    result = {};
    result.outcome = Invalid("schema7031_request", "request_extent_drifted");
    return result;
  }
  result.request = request;
  return result;
}

PsNarrowStatementContextRequestCodecResultV1
DecodeAndValidatePsNarrowStatementContextRequestV1(
    std::span<const byte> payload,
    const PsNarrowStatementContextRequestValidationContextV1& context) {
  PsNarrowStatementContextRequestCodecResultV1 result;
  result.outcome = ValidateRequestContext(context);
  if (!result.outcome.ok()) return result;
  if (payload.size() != kPsStatementContextRequestBytesV11 ||
      LoadU16(payload, 0) != kPsStatementContextProjectionVersionV11) {
    result.outcome = Invalid("schema7031_request",
                             "request_version_or_extent_invalid");
    return result;
  }
  PsNarrowStatementContextRequestV1 decoded;
  decoded.session_uuid = LoadUuid(payload, 2);
  decoded.owning_local_transaction_id = LoadU64(payload, 18);
  decoded.owning_transaction_uuid = LoadUuid(payload, 26);
  result.outcome = ValidateRequest(decoded, context);
  if (!result.outcome.ok()) return result;
  const auto canonical = EncodeAndValidatePsNarrowStatementContextRequestV1(
      decoded, context);
  if (!canonical.ok() || canonical.canonical_payload.size() != payload.size() ||
      !std::equal(canonical.canonical_payload.begin(),
                  canonical.canonical_payload.end(), payload.begin())) {
    result.outcome = Invalid("schema7031_request",
                             "request_bytes_are_not_canonical");
    return result;
  }
  result.canonical_payload = canonical.canonical_payload;
  result.request = decoded;
  return result;
}

PsNarrowStatementContextRequestCodecResultV1
ValidatePsNarrowStatementContextRequestAliasIdentityV1(
    std::span<const byte> schema7709_payload,
    std::span<const byte> canonical_schema7031_v11_payload,
    const PsNarrowStatementContextRequestValidationContextV1& context) {
  PsNarrowStatementContextRequestCodecResultV1 result;
  if (schema7709_payload.size() != canonical_schema7031_v11_payload.size() ||
      !std::equal(schema7709_payload.begin(), schema7709_payload.end(),
                  canonical_schema7031_v11_payload.begin())) {
    result.outcome = Error(
        PsStatementContextAliasStatusV1::alias_bytes_mismatch,
        kFrameInvalid, "schema7709_payload",
        "alias_bytes_differ_from_canonical_schema7031_v11_source");
    return result;
  }
  return DecodeAndValidatePsNarrowStatementContextRequestV1(
      schema7709_payload, context);
}

PsNarrowStatementContextResultAliasResultV1
ValidateAndAdoptPsNarrowStatementContextResultAliasV1(
    std::span<const byte> schema7710_payload,
    std::span<const byte> canonical_schema7032_v71_payload) {
  PsNarrowStatementContextResultAliasResultV1 result;
  if (schema7710_payload.empty() ||
      schema7710_payload.size() != canonical_schema7032_v71_payload.size() ||
      !std::equal(schema7710_payload.begin(), schema7710_payload.end(),
                  canonical_schema7032_v71_payload.begin())) {
    result.outcome = Error(
        PsStatementContextAliasStatusV1::alias_bytes_mismatch,
        kFrameInvalid, "schema7710_payload",
        "alias_bytes_differ_from_canonical_schema7032_v71_source");
    return result;
  }
  std::size_t extension = 0;
  PsStatementContextUuidV1 transaction_uuid{};
  PsStatementContextUuidV1 statement_snapshot_uuid{};
  std::uint64_t local_transaction_id = 0;
  result.outcome = LocateAndValidateSchema7032Base(
      canonical_schema7032_v71_payload, &extension, &transaction_uuid,
      &local_transaction_id, &statement_snapshot_uuid);
  if (!result.outcome.ok()) return result;
  PsNarrowStatementContextResultSummaryV1 summary;
  result.outcome = ValidateSchema7032ExtensionV71(
      canonical_schema7032_v71_payload, extension, transaction_uuid,
      local_transaction_id, statement_snapshot_uuid, &summary);
  if (!result.outcome.ok()) return result;
  result.canonical_payload.assign(schema7710_payload.begin(),
                                  schema7710_payload.end());
  result.summary = summary;
  return result;
}

const char* PsStatementContextAliasStatusNameV1(
    PsStatementContextAliasStatusV1 status) {
  switch (status) {
    case PsStatementContextAliasStatusV1::ok:
      return "ok";
    case PsStatementContextAliasStatusV1::invalid_argument:
      return "invalid_argument";
    case PsStatementContextAliasStatusV1::resource_limit_exceeded:
      return "resource_limit_exceeded";
    case PsStatementContextAliasStatusV1::source_schema_invalid:
      return "source_schema_invalid";
    case PsStatementContextAliasStatusV1::alias_bytes_mismatch:
      return "alias_bytes_mismatch";
    case PsStatementContextAliasStatusV1::request_authority_mismatch:
      return "request_authority_mismatch";
  }
  return "unknown";
}

}  // namespace scratchbird::parser::ipc
