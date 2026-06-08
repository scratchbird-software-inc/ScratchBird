// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "runtime_platform.hpp"

#include <array>
#include <string>

namespace scratchbird::core::uuid {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::Uuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u8;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

struct UuidResult {
  Status status;
  Uuid value;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct UuidParseResult {
  Status status;
  Uuid value;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct TypedUuidResult {
  Status status;
  TypedUuid value;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct UuidVersionPolicy {
  bool allow_v1 = true;
  bool allow_v2 = true;
  bool allow_v3 = true;
  bool allow_v4 = true;
  bool allow_v5 = true;
  bool allow_v6 = true;
  bool allow_v7 = true;
};

struct UuidV7TimePrefixResult {
  bool ok = false;
  bool fallback_to_uncompressed_uuid = true;
  u64 unix_epoch_millis = 0;
  std::string refusal_reason;
};

struct UuidV7IndexCompareResult {
  bool ok = false;
  bool specialized_comparator_used = false;
  bool fallback_to_uncompressed_uuid = true;
  int comparison = 0;
  std::string refusal_reason;
};

const char* UuidKindName(UuidKind kind);
const char* UuidVersionName(u8 version);
bool UuidKindAllowsDurableIdentity(UuidKind kind);
bool IsEngineIdentityKind(UuidKind kind);
bool IsDurableEngineIdentityKind(UuidKind kind);
Uuid NilUuid();
bool IsNilUuid(const Uuid& uuid);
bool IsValidUuidVariant(const Uuid& uuid);
u8 UuidVersion(const Uuid& uuid);
bool IsEngineIdentityUuid(const Uuid& uuid);
bool UuidVersionAllowed(const Uuid& uuid, const UuidVersionPolicy& policy);
int CompareUuid128(const Uuid& left, const Uuid& right);
UuidV7TimePrefixResult ExtractUuidV7TimePrefix(const Uuid& uuid);
UuidV7IndexCompareResult CompareUuidV7ForIndex(const TypedUuid& left,
                                               const TypedUuid& right,
                                               UuidKind expected_kind);
std::string UuidToString(const Uuid& uuid);
UuidParseResult ParseUuid(std::string text);
TypedUuidResult MakeTypedUuid(UuidKind kind, Uuid value);
TypedUuidResult ParseTypedUuid(UuidKind kind, std::string text);
TypedUuidResult MakeDurableEngineIdentityUuid(UuidKind kind, Uuid value);
TypedUuidResult ParseDurableEngineIdentityUuid(UuidKind kind, std::string text);
TypedUuidResult GenerateEngineIdentityV7(UuidKind kind, u64 unix_epoch_millis);
TypedUuidResult GenerateDurableEngineIdentityV7(UuidKind kind, u64 unix_epoch_millis);

UuidResult GenerateCompatibilityTimeNodeV1(u64 gregorian_100ns_timestamp,
                                           u16 clock_sequence,
                                           std::array<byte, 6> node);
UuidResult GenerateCompatibilityDceSecurityV2(u64 gregorian_100ns_timestamp,
                                              u16 clock_sequence,
                                              std::array<byte, 6> node,
                                              u8 local_domain,
                                              u32 local_identifier);
UuidResult GenerateCompatibilityNameBasedV3(const Uuid& namespace_uuid, std::string name);
UuidResult GenerateCompatibilityRandomV4();
UuidResult GenerateCompatibilityNameBasedV5(const Uuid& namespace_uuid, std::string name);
UuidResult GenerateCompatibilityReorderedTimeV6(u64 gregorian_100ns_timestamp,
                                                u16 clock_sequence,
                                                std::array<byte, 6> node);
UuidResult GenerateCompatibilityUnixTimeV7(u64 unix_epoch_millis);

DiagnosticRecord MakeUuidDiagnostic(Status status,
                                    std::string diagnostic_code,
                                    std::string message_key,
                                    std::string detail = {});

}  // namespace scratchbird::core::uuid
