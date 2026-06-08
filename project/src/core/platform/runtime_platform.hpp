// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::core::platform {

using i8 = std::int8_t;
using u8 = std::uint8_t;
using i16 = std::int16_t;
using u16 = std::uint16_t;
using i32 = std::int32_t;
using u32 = std::uint32_t;
using i64 = std::int64_t;
using u64 = std::uint64_t;
using usize = std::size_t;
using byte = std::uint8_t;

#if defined(__SIZEOF_INT128__)
using i128 = __int128_t;
using u128 = __uint128_t;
inline constexpr bool kCompilerHasInt128 = true;
#else
inline constexpr bool kCompilerHasInt128 = false;
#endif

static_assert(sizeof(i8) == 1, "ScratchBird requires 8-bit signed integers");
static_assert(sizeof(u8) == 1, "ScratchBird requires 8-bit unsigned integers");
static_assert(sizeof(i16) == 2, "ScratchBird requires 16-bit signed integers");
static_assert(sizeof(u16) == 2, "ScratchBird requires 16-bit unsigned integers");
static_assert(sizeof(i32) == 4, "ScratchBird requires 32-bit signed integers");
static_assert(sizeof(u32) == 4, "ScratchBird requires 32-bit unsigned integers");
static_assert(sizeof(i64) == 8, "ScratchBird requires 64-bit signed integers");
static_assert(sizeof(u64) == 8, "ScratchBird requires 64-bit unsigned integers");
static_assert(kCompilerHasInt128, "ScratchBird requires compiler support for int128 and uint128 primitives");

#if defined(__SIZEOF_INT128__)
static_assert(sizeof(i128) == 16, "ScratchBird requires 128-bit signed integers");
static_assert(sizeof(u128) == 16, "ScratchBird requires 128-bit unsigned integers");
#endif

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && defined(__ORDER_BIG_ENDIAN__)
enum class HostEndian : u8 {
  little,
  big
};

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
inline constexpr HostEndian kHostEndian = HostEndian::little;
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
inline constexpr HostEndian kHostEndian = HostEndian::big;
#else
#error "ScratchBird requires a known little-endian or big-endian target"
#endif
#else
#error "ScratchBird requires compiler byte-order macros"
#endif

constexpr u16 ByteSwap16(u16 value) {
  return static_cast<u16>((value >> 8) | (value << 8));
}

constexpr u32 ByteSwap32(u32 value) {
  return ((value & 0x000000ffu) << 24) |
         ((value & 0x0000ff00u) << 8) |
         ((value & 0x00ff0000u) >> 8) |
         ((value & 0xff000000u) >> 24);
}

constexpr u64 ByteSwap64(u64 value) {
  return ((value & 0x00000000000000ffull) << 56) |
         ((value & 0x000000000000ff00ull) << 40) |
         ((value & 0x0000000000ff0000ull) << 24) |
         ((value & 0x00000000ff000000ull) << 8) |
         ((value & 0x000000ff00000000ull) >> 8) |
         ((value & 0x0000ff0000000000ull) >> 24) |
         ((value & 0x00ff000000000000ull) >> 40) |
         ((value & 0xff00000000000000ull) >> 56);
}

constexpr u16 HostToLittle16(u16 value) {
  return kHostEndian == HostEndian::little ? value : ByteSwap16(value);
}

constexpr u32 HostToLittle32(u32 value) {
  return kHostEndian == HostEndian::little ? value : ByteSwap32(value);
}

constexpr u64 HostToLittle64(u64 value) {
  return kHostEndian == HostEndian::little ? value : ByteSwap64(value);
}

constexpr u16 LittleToHost16(u16 value) {
  return HostToLittle16(value);
}

constexpr u32 LittleToHost32(u32 value) {
  return HostToLittle32(value);
}

constexpr u64 LittleToHost64(u64 value) {
  return HostToLittle64(value);
}

inline u16 LoadLittle16(const void* source) {
  u16 value = 0;
  std::memcpy(&value, source, sizeof(value));
  return LittleToHost16(value);
}

inline u32 LoadLittle32(const void* source) {
  u32 value = 0;
  std::memcpy(&value, source, sizeof(value));
  return LittleToHost32(value);
}

inline u64 LoadLittle64(const void* source) {
  u64 value = 0;
  std::memcpy(&value, source, sizeof(value));
  return LittleToHost64(value);
}

inline void StoreLittle16(void* target, u16 value) {
  const u16 stored = HostToLittle16(value);
  std::memcpy(target, &stored, sizeof(stored));
}

inline void StoreLittle32(void* target, u32 value) {
  const u32 stored = HostToLittle32(value);
  std::memcpy(target, &stored, sizeof(stored));
}

inline void StoreLittle64(void* target, u64 value) {
  const u64 stored = HostToLittle64(value);
  std::memcpy(target, &stored, sizeof(stored));
}

enum class Severity : u8 {
  trace = 1,
  info = 2,
  warning = 3,
  error = 4,
  fatal = 5
};

enum class Subsystem : u16 {
  platform = 1,
  diagnostics = 2,
  uuid = 3,
  time = 4,
  memory = 10,
  storage_disk = 20,
  storage_page = 21,
  datatypes = 30,
  transaction_mga = 40,
  catalog = 50,
  engine = 60,
  parser = 70,
  cluster_private = 900
};

enum class StatusCode : u32 {
  ok = 0,
  platform_compile_gate_failed = 100001,
  platform_unknown_endian = 100002,
  platform_required_feature_missing = 100003,
  diagnostic_invalid_record = 100100,
  uuid_invalid = 100200,
  time_source_unavailable = 100300,
  memory_invalid_request = 100400,
  memory_allocation_failed = 100401,
  memory_limit_exceeded = 100402,
  memory_unknown_pointer = 100403
};

struct Status {
  StatusCode code = StatusCode::ok;
  Severity severity = Severity::info;
  Subsystem subsystem = Subsystem::platform;

  constexpr bool ok() const {
    return code == StatusCode::ok;
  }
};

struct DiagnosticArgument {
  std::string key;
  std::string value;
};

struct DiagnosticRecord {
  Status status;
  std::string diagnostic_code;
  std::string message_key;
  std::vector<DiagnosticArgument> arguments;
  std::string trace_id;
  std::string source_component;
  std::string remediation_hint;
};

struct DiagnosticValidationResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<DiagnosticArgument> failures;

  bool ok() const {
    return status.ok() && failures.empty();
  }
};

enum class UuidKind : u8 {
  database,
  cluster,
  filespace,
  schema,
  object,
  row,
  page,
  transaction,
  session,
  principal,
  unknown
};

struct Uuid {
  std::array<byte, 16> bytes{};

  constexpr bool is_nil() const {
    for (byte value : bytes) {
      if (value != 0) {
        return false;
      }
    }
    return true;
  }

  friend constexpr bool operator==(const Uuid& left, const Uuid& right) {
    return left.bytes == right.bytes;
  }

  friend constexpr bool operator!=(const Uuid& left, const Uuid& right) {
    return !(left == right);
  }
};

struct TypedUuid {
  UuidKind kind = UuidKind::unknown;
  Uuid value;

  constexpr bool valid() const {
    return kind != UuidKind::unknown && !value.is_nil();
  }
};

struct MonotonicTime {
  u64 ticks = 0;
};

struct WallClockTime {
  i64 unix_seconds = 0;
  u32 nanoseconds = 0;
};

struct StandardizedClusterTime {
  WallClockTime observed_time;
  u64 uncertainty_nanoseconds = 0;
  bool authoritative_for_visibility = false;
};

struct CompileFeatureGates {
  bool cxx17_or_newer = false;
  bool compiler_int128 = false;
  bool endian_known = false;
  bool exceptions_enabled = false;
  bool rtti_enabled = false;
};

CompileFeatureGates CurrentCompileFeatureGates();
Status CheckCompileFeatureGates();
const char* SeverityName(Severity severity);
const char* SubsystemName(Subsystem subsystem);
const char* StatusCodeName(StatusCode code);
DiagnosticRecord MakeDiagnostic(StatusCode code,
                                Severity severity,
                                Subsystem subsystem,
                                std::string diagnostic_code,
                                std::string message_key,
                                std::vector<DiagnosticArgument> arguments = {},
                                std::string trace_id = {},
                                std::string source_component = {},
                                std::string remediation_hint = {});
bool DiagnosticCodeLooksCanonical(const std::string& diagnostic_code);
bool DiagnosticMessageKeyLooksCanonical(const std::string& message_key);
bool DiagnosticRecordHasPlaceholderText(const DiagnosticRecord& record);
DiagnosticValidationResult ValidateDiagnosticRecord(const DiagnosticRecord& record);

}  // namespace scratchbird::core::platform
