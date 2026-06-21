// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "control_plane.hpp"
#include "savepoint.hpp"
#include "transaction_manager.hpp"
#include "uuid.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <errno.h>
#include <unistd.h>
#endif

namespace {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::u64;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::GenerateEngineIdentityV7;
using scratchbird::transaction::mga::LocalTransactionId;
using scratchbird::transaction::mga::LocalTransactionManager;
using scratchbird::transaction::mga::SavepointStack;

constexpr std::size_t kSbwpHeaderSize = 40;
constexpr std::uint8_t kSbwpMajor = 1;
constexpr std::uint8_t kSbwpMinor = 1;
constexpr std::uint16_t kSbwpVersionMin = 0x0100;
constexpr std::uint16_t kSbwpVersionCurrent = 0x0101;
constexpr std::uint32_t kMaxPayloadBytes = 64u * 1024u * 1024u;
constexpr std::uint8_t kFrameFlagCompressed = 1u << 0;
constexpr std::uint8_t kFrameFlagPartial = 1u << 1;
constexpr std::uint8_t kFrameFlagKnownMask = kFrameFlagCompressed | kFrameFlagPartial;
constexpr std::uint32_t kOidInt8 = 20;

enum Msg : std::uint8_t {
  kStartup = 0x01,
  kQuery = 0x03,
  kParse = 0x04,
  kBind = 0x05,
  kDescribe = 0x06,
  kExecute = 0x07,
  kClose = 0x08,
  kSync = 0x09,
  kCancel = 0x0b,
  kTerminate = 0x0c,
  kCopyData = 0x0d,
  kCopyDone = 0x0e,
  kCopyFail = 0x0f,
  kSblrExecute = 0x10,
  kSubscribe = 0x11,
  kUnsubscribe = 0x12,
  kStreamControl = 0x14,
  kTxnBegin = 0x15,
  kTxnCommit = 0x16,
  kTxnRollback = 0x17,
  kTxnSavepoint = 0x18,
  kTxnRelease = 0x19,
  kTxnRollbackTo = 0x1a,
  kPing = 0x1b,
  kSetOption = 0x1c,
  kResetSession = 0x21,
  kReauth = 0x22,
  kTraceContext = 0x23,

  kAuthOk = 0x41,
  kReady = 0x43,
  kRowDescription = 0x44,
  kDataRow = 0x45,
  kCommandComplete = 0x46,
  kParseComplete = 0x47,
  kError = 0x48,
  kCloseComplete = 0x4c,
  kParameterStatus = 0x4f,
  kParameterDescription = 0x50,
  kNotification = 0x54,
  kPong = 0x5d,
  kServerInfo = 0x61,
  kCancelAck = 0x65,
  kCancelled = 0x66,
  kBulkRejectData = 0x6e,
  kLobLocator = 0x6f,
  kLobChunk = 0x70,
  kLobClose = 0x71,
  kHeartbeat = 0x80,
  kExtension = 0x81,
};

enum class ReadyReason : std::uint8_t {
  kStartup = 1,
  kCommandComplete = 2,
  kErrorRecovered = 3,
  kResetComplete = 4,
  kReauthComplete = 5,
  kCancelOutcome = 6,
  kStateChange = 7,
};

constexpr std::uint64_t FeatureBit(unsigned bit) {
  return 1ull << bit;
}

constexpr std::uint64_t kFeatureCompression = FeatureBit(0);
constexpr std::uint64_t kFeatureStreaming = FeatureBit(1);
constexpr std::uint64_t kFeatureSblr = FeatureBit(2);
constexpr std::uint64_t kFeatureNotifications = FeatureBit(4);
constexpr std::uint64_t kFeatureBatch = FeatureBit(6);
constexpr std::uint64_t kFeaturePipeline = FeatureBit(7);
constexpr std::uint64_t kFeatureBinaryCopy = FeatureBit(8);
constexpr std::uint64_t kFeatureSavepoints = FeatureBit(9);
constexpr std::uint64_t kFeatureMultiResult = FeatureBit(13);
constexpr std::uint64_t kFeatureGeneratedKeys = FeatureBit(14);
constexpr std::uint64_t kFeatureOutParameters = FeatureBit(15);
constexpr std::uint64_t kFeatureArrayBind = FeatureBit(16);
constexpr std::uint64_t kFeatureBulkRejects = FeatureBit(17);
constexpr std::uint64_t kFeatureLobLocator = FeatureBit(18);
constexpr std::uint64_t kFeatureCursors = FeatureBit(19);
constexpr std::uint64_t kFeatureCopyBackpressure = FeatureBit(20);
constexpr std::uint64_t kFeatureSessionReset = FeatureBit(21);
constexpr std::uint64_t kFeatureReauth = FeatureBit(22);
constexpr std::uint64_t kFeatureFailoverHints = FeatureBit(23);
constexpr std::uint64_t kFeatureTraceContext = FeatureBit(24);
constexpr std::uint64_t kKnownCoreFeatureMask = FeatureBit(25) - 1u;
constexpr std::uint64_t kP1OnlyFeatureMask =
    kFeatureMultiResult | kFeatureGeneratedKeys | kFeatureOutParameters |
    kFeatureArrayBind | kFeatureBulkRejects | kFeatureLobLocator | kFeatureCursors |
    kFeatureCopyBackpressure | kFeatureSessionReset | kFeatureReauth |
    kFeatureFailoverHints | kFeatureTraceContext;
constexpr std::uint64_t kServerSupportedFeatureMask =
    kFeatureNotifications | kFeatureBatch | kFeaturePipeline | kFeatureSavepoints |
    kFeatureMultiResult | kFeatureGeneratedKeys | kFeatureOutParameters |
    kFeatureBulkRejects | kFeatureLobLocator | kFeatureCursors |
    kFeatureSessionReset | kFeatureReauth | kFeatureTraceContext;
constexpr std::uint32_t kConnectFlagDormantReattach = 1u << 0;
constexpr std::uint32_t kConnectFlagBoundDbUuid = 1u << 1;
constexpr std::uint32_t kConnectFlagSqlCompileAssist = 1u << 2;
constexpr std::uint32_t kConnectFlagManagerDbbt = 1u << 3;
constexpr std::uint32_t kConnectFlagMultiplexRequest = 1u << 4;
constexpr std::uint32_t kKnownConnectFlagMask =
    kConnectFlagDormantReattach | kConnectFlagBoundDbUuid |
    kConnectFlagSqlCompileAssist | kConnectFlagManagerDbbt |
    kConnectFlagMultiplexRequest;

struct Header {
  std::uint8_t msg_type{0};
  std::uint8_t flags{0};
  std::uint32_t length{0};
  std::uint32_t sequence{0};
  std::array<std::uint8_t, 16> attachment_id{};
  std::uint64_t txn_id{0};
};

struct Frame {
  Header header;
  std::vector<std::uint8_t> payload;
};

struct PreparedStatement {
  std::string sql;
  std::vector<std::uint32_t> param_types;
};

struct SessionState {
  std::array<std::uint8_t, 16> attachment_id{};
  std::array<std::uint8_t, 16> session_uuid{};
  std::uint32_t server_sequence{0};
  std::uint64_t txn_id{0};
  bool p1_payloads{false};
  std::uint16_t selected_protocol_version{kSbwpVersionCurrent};
  std::uint64_t negotiated_features{0};
  std::map<std::string, std::string> session_parameters;
  bool autocommit{true};
  LocalTransactionManager transaction_manager;
  LocalTransactionId active_transaction;
  SavepointStack savepoints;
  std::map<std::string, PreparedStatement> statements;
  std::string bound_sql;
  std::vector<std::uint32_t> bound_param_types;
};

std::uint16_t ReadU16(const std::vector<std::uint8_t>& data, std::size_t off) {
  if (off + 2 > data.size()) return 0;
  return static_cast<std::uint16_t>(data[off]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[off + 1]) << 8u);
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& data, std::size_t off) {
  if (off + 4 > data.size()) return 0;
  std::uint32_t out = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    out |= static_cast<std::uint32_t>(data[off + static_cast<std::size_t>(shift / 8)]) << shift;
  }
  return out;
}

std::uint64_t ReadU64(const std::vector<std::uint8_t>& data, std::size_t off) {
  if (off + 8 > data.size()) return 0;
  std::uint64_t out = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    out |= static_cast<std::uint64_t>(data[off + static_cast<std::size_t>(shift / 8)]) << shift;
  }
  return out;
}

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void PutU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void PutI16(std::vector<std::uint8_t>* out, std::int16_t value) {
  PutU16(out, static_cast<std::uint16_t>(value));
}

void PutI32(std::vector<std::uint8_t>* out, std::int32_t value) {
  PutU32(out, static_cast<std::uint32_t>(value));
}

void PutUuid(std::vector<std::uint8_t>* out, const std::array<std::uint8_t, 16>& uuid) {
  out->insert(out->end(), uuid.begin(), uuid.end());
}

void PutZeroUuid(std::vector<std::uint8_t>* out) {
  out->insert(out->end(), 16, 0);
}

void PutLpStr(std::vector<std::uint8_t>* out, std::string_view value) {
  PutU32(out, static_cast<std::uint32_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

bool ReadLpStr(const std::vector<std::uint8_t>& payload,
               std::size_t* off,
               std::string* value) {
  if (*off + 4 > payload.size()) return false;
  const auto length = ReadU32(payload, *off);
  *off += 4;
  if (*off + length > payload.size()) return false;
  value->assign(reinterpret_cast<const char*>(payload.data() + *off), length);
  *off += length;
  return true;
}

void PutNullableText(std::vector<std::uint8_t>* out, std::string_view value) {
  out->push_back(3);
  PutU32(out, static_cast<std::uint32_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

void PutAbsentNullableText(std::vector<std::uint8_t>* out) {
  out->push_back(0);
  PutU32(out, 0);
}

void PutHash256Zero(std::vector<std::uint8_t>* out) {
  out->insert(out->end(), 32, 0);
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string Trim(std::string value) {
  auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

std::string ReadSizedString(const std::vector<std::uint8_t>& payload, std::size_t* off) {
  if (*off + 4 > payload.size()) return {};
  const std::uint32_t size = ReadU32(payload, *off);
  *off += 4;
  if (*off + size > payload.size()) {
    *off = payload.size();
    return {};
  }
  std::string out(reinterpret_cast<const char*>(payload.data() + *off), size);
  *off += size;
  return out;
}

std::string ReadNullTerminatedSql(const std::vector<std::uint8_t>& payload, std::size_t off) {
  if (off >= payload.size()) return {};
  std::size_t end = off;
  while (end < payload.size() && payload[end] != 0) ++end;
  return std::string(reinterpret_cast<const char*>(payload.data() + off), end - off);
}

std::optional<PreparedStatement> ParsePreparedStatement(const std::vector<std::uint8_t>& payload,
                                                        std::string* name) {
  std::size_t off = 0;
  *name = ReadSizedString(payload, &off);
  std::string sql = ReadSizedString(payload, &off);
  if (off + 4 > payload.size()) return std::nullopt;
  const std::uint16_t param_count = ReadU16(payload, off);
  off += 4;
  PreparedStatement prepared;
  prepared.sql = std::move(sql);
  prepared.param_types.reserve(param_count);
  for (std::uint16_t i = 0; i < param_count && off + 4 <= payload.size(); ++i) {
    prepared.param_types.push_back(ReadU32(payload, off));
    off += 4;
  }
  return prepared;
}

std::string ParseBindStatementName(const std::vector<std::uint8_t>& payload) {
  std::size_t off = 0;
  (void)ReadSizedString(payload, &off);
  return ReadSizedString(payload, &off);
}

std::string ParseQuerySql(const std::vector<std::uint8_t>& payload) {
  if (payload.size() < 12) return {};
  return ReadNullTerminatedSql(payload, 12);
}

bool LooksLikeP1Startup(const std::vector<std::uint8_t>& payload) {
  if (payload.size() < 84) return false;
  // Legacy startup begins with nul-terminated ASCII keys. P1 startup begins with
  // little-endian version words; unsupported P1 windows must fail closed here.
  return payload[1] <= 0x02u && payload[3] <= 0x02u;
}

bool IsKnownConnectKey(std::string_view key) {
  return key == "application_name" || key == "calendar" || key == "charset" ||
         key == "client_encoding" || key == "database" || key == "decimal_locale" ||
         key == "default_catalog" || key == "default_schema" ||
         key == "keepalive_interval_ms" || key == "keepalive_timeout_ms" ||
         key == "language" || key == "statement_timeout_ms" || key == "timezone" ||
         key == "time_zone" || key == "traceparent" || key == "tracestate" || key == "user";
}

struct StartupNegotiation {
  bool p1 = false;
  bool ok = true;
  std::string sqlstate = "08P01";
  std::string message;
  std::string detail;
  std::uint16_t selected_protocol_version = kSbwpVersionCurrent;
  std::uint64_t negotiated_features = 0;
  std::map<std::string, std::string> params;
};

StartupNegotiation RejectStartup(std::string sqlstate,
                                 std::string message,
                                 std::string detail) {
  StartupNegotiation rejected;
  rejected.p1 = true;
  rejected.ok = false;
  rejected.sqlstate = std::move(sqlstate);
  rejected.message = std::move(message);
  rejected.detail = std::move(detail);
  return rejected;
}

std::string DecodeConnectValue(std::uint8_t value_type,
                               const std::vector<std::uint8_t>& payload,
                               std::size_t value_offset,
                               std::uint32_t value_length) {
  if (value_offset + value_length > payload.size()) return {};
  const auto* data = payload.data() + value_offset;
  if (value_type == 0x02 && value_length == 8) return std::to_string(ReadU64(payload, value_offset));
  if (value_type == 0x02 && value_length == 4) return std::to_string(ReadU32(payload, value_offset));
  if (value_type == 0x03) return value_length > 0 && data[0] != 0 ? "true" : "false";
  return std::string(reinterpret_cast<const char*>(data), value_length);
}

StartupNegotiation ParseStartupNegotiation(const std::vector<std::uint8_t>& payload) {
  StartupNegotiation negotiated;
  if (!LooksLikeP1Startup(payload)) return negotiated;
  negotiated.p1 = true;
  const auto min_version = ReadU16(payload, 0);
  const auto max_version = ReadU16(payload, 2);
  const auto connect_flags = ReadU32(payload, 4);
  const auto client_features = ReadU64(payload, 8);
  const auto required_features = ReadU64(payload, 16);
  const auto optional_features = ReadU64(payload, 24);
  if (min_version > max_version || max_version < kSbwpVersionMin ||
      min_version > kSbwpVersionCurrent) {
    return RejectStartup("08P01",
                         "SBWP.VERSION.NO_COMMON_VERSION",
                         "client and native worker protocol windows do not overlap");
  }
  if ((connect_flags & ~kKnownConnectFlagMask) != 0) {
    return RejectStartup("08P01",
                         "NATIVE_WIRE.CONNECT_INVALID_PAYLOAD",
                         "startup connect_flags contains reserved bits");
  }
  if ((connect_flags & kConnectFlagDormantReattach) != 0 &&
      (connect_flags & kConnectFlagMultiplexRequest) != 0) {
    return RejectStartup("08P01",
                         "NATIVE_WIRE.CONNECT_INVALID_PAYLOAD",
                         "dormant reattach and multiplex connect flags are mutually exclusive");
  }
  negotiated.selected_protocol_version = std::min<std::uint16_t>(max_version, kSbwpVersionCurrent);
  if (negotiated.selected_protocol_version < kSbwpVersionCurrent &&
      (required_features & kP1OnlyFeatureMask) != 0) {
    return RejectStartup("0A000",
                         "SBWP.FEATURE.REQUIRED_UNSUPPORTED",
                         "required P1 native-wire feature is unavailable on selected downgrade");
  }
  if ((required_features & ~kKnownCoreFeatureMask) != 0) {
    return RejectStartup("08P01",
                         "SBWP.FEATURE.UNKNOWN_REQUIRED",
                         "startup required an unknown core feature bit");
  }
  if ((required_features & ~client_features) != 0) {
    return RejectStartup("08P01",
                         "NATIVE_WIRE.CONNECT_INVALID_PAYLOAD",
                         "startup required a feature absent from client_feature_bitmap");
  }
  if ((required_features & ~kServerSupportedFeatureMask) != 0) {
    return RejectStartup("0A000",
                         "SBWP.FEATURE.REQUIRED_UNSUPPORTED",
                         "startup required an unsupported native worker feature");
  }
  auto supported = kServerSupportedFeatureMask;
  if (negotiated.selected_protocol_version < kSbwpVersionCurrent) supported &= ~kP1OnlyFeatureMask;
  negotiated.negotiated_features = client_features & supported;
  (void)optional_features;
  std::size_t off = 80;
  const auto count = ReadU32(payload, off);
  off += 4;
  if (count > 256) return RejectStartup("08P01", "NATIVE_WIRE.CONNECT_INVALID_PAYLOAD", "too many connect keys");
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string key;
    if (!ReadLpStr(payload, &off, &key) || off + 6 > payload.size()) {
      return RejectStartup("08P01", "NATIVE_WIRE.CONNECT_INVALID_PAYLOAD", "truncated connect key");
    }
    const auto value_type = payload[off++];
    const auto redaction_class = payload[off++];
    const auto value_length = ReadU32(payload, off);
    off += 4;
    if (off + value_length > payload.size() || !IsKnownConnectKey(key)) {
      return RejectStartup("08P01",
                           IsKnownConnectKey(key) ? "NATIVE_WIRE.CONNECT_INVALID_PAYLOAD"
                                                  : "NATIVE_WIRE.CONNECT_UNKNOWN_KEY",
                           "connect key is malformed or not registered");
    }
    if (redaction_class < 3) negotiated.params[key] = DecodeConnectValue(value_type, payload, off, value_length);
    off += value_length;
  }
  if (off + 4 > payload.size()) {
    return RejectStartup("08P01", "NATIVE_WIRE.CONNECT_INVALID_PAYLOAD", "missing extension offer count");
  }
  const auto extension_count = ReadU32(payload, off);
  off += 4;
  for (std::uint32_t i = 0; i < extension_count; ++i) {
    std::string name;
    if (!ReadLpStr(payload, &off, &name) || off + 10 > payload.size()) {
      return RejectStartup("08P01", "SBWP.EXTENSION.REQUIRED_UNSUPPORTED", "truncated extension offer");
    }
    off += 6;
    const auto flags = ReadU32(payload, off);
    off += 4;
    if ((flags & ~3u) != 0 || ((flags & 1u) != 0 && (flags & 2u) != 0)) {
      return RejectStartup("08P01", "SBWP.EXTENSION.REQUIRED_UNSUPPORTED", "invalid extension flags");
    }
    if ((flags & 1u) != 0) {
      return RejectStartup("08P01", "SBWP.EXTENSION.UNKNOWN_REQUIRED", "required extension is not registered");
    }
  }
  if (off != payload.size()) {
    return RejectStartup("08P01", "NATIVE_WIRE.CONNECT_INVALID_PAYLOAD", "startup has trailing bytes");
  }
  return negotiated;
}

std::uint64_t CountPlaceholders(std::string_view sql) {
  std::uint64_t max_param = 0;
  bool in_single = false;
  bool in_double = false;
  for (std::size_t i = 0; i + 1 < sql.size(); ++i) {
    const char ch = sql[i];
    if (ch == '\'' && !in_double) {
      in_single = !in_single;
      continue;
    }
    if (ch == '"' && !in_single) {
      in_double = !in_double;
      continue;
    }
    if (in_single || in_double || ch != '$' || !std::isdigit(static_cast<unsigned char>(sql[i + 1]))) continue;
    std::uint64_t value = 0;
    std::size_t j = i + 1;
    while (j < sql.size() && std::isdigit(static_cast<unsigned char>(sql[j]))) {
      value = (value * 10u) + static_cast<std::uint64_t>(sql[j] - '0');
      ++j;
    }
    max_param = std::max(max_param, value);
    i = j;
  }
  return max_param;
}

bool StartsWithWord(std::string_view text, std::string_view word) {
  if (text.size() < word.size() || text.substr(0, word.size()) != word) return false;
  return text.size() == word.size() ||
         (!std::isalnum(static_cast<unsigned char>(text[word.size()])) && text[word.size()] != '_');
}

std::string StripSqlTerminator(std::string sql) {
  sql = Trim(std::move(sql));
  while (!sql.empty() && sql.back() == ';') {
    sql.pop_back();
    sql = Trim(std::move(sql));
  }
  return sql;
}

std::string FirstTokenAfter(std::string_view sql, std::string_view marker) {
  const std::string lowered = Lower(std::string(sql));
  const auto pos = lowered.find(marker);
  if (pos == std::string::npos) return {};
  std::size_t off = pos + marker.size();
  while (off < sql.size() && std::isspace(static_cast<unsigned char>(sql[off]))) ++off;
  if (off >= sql.size()) return {};
  char quote = 0;
  if (sql[off] == '"' || sql[off] == '\'') quote = sql[off++];
  std::string out;
  while (off < sql.size()) {
    const char ch = sql[off++];
    if (quote != 0) {
      if (ch == quote) break;
      out.push_back(ch);
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(ch)) || ch == ';') break;
    out.push_back(ch);
  }
  return out;
}

u64 CurrentUnixEpochMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::string DiagnosticText(const DiagnosticRecord& diagnostic) {
  if (!diagnostic.message_key.empty()) return diagnostic.message_key;
  if (!diagnostic.diagnostic_code.empty()) return diagnostic.diagnostic_code;
  if (!diagnostic.remediation_hint.empty()) return diagnostic.remediation_hint;
  return "transaction manager rejected the request";
}

bool BeginTransaction(SessionState* state, std::string* error) {
  if (state->active_transaction.valid()) return true;
  auto uuid = GenerateEngineIdentityV7(UuidKind::transaction, CurrentUnixEpochMillis());
  if (!uuid.ok()) {
    *error = DiagnosticText(uuid.diagnostic);
    return false;
  }
  auto result = state->transaction_manager.Begin(uuid.value, CurrentUnixEpochMillis());
  if (!result.ok()) {
    *error = DiagnosticText(result.diagnostic);
    return false;
  }
  state->active_transaction = result.entry.identity.local_id;
  state->txn_id = state->active_transaction.value;
  return true;
}

bool CommitActiveTransaction(SessionState* state, std::string* error) {
  if (!state->active_transaction.valid()) {
    state->txn_id = 0;
    return true;
  }
  auto result = state->transaction_manager.Commit(state->active_transaction, CurrentUnixEpochMillis());
  if (!result.ok()) {
    *error = DiagnosticText(result.diagnostic);
    return false;
  }
  state->active_transaction = {};
  state->txn_id = 0;
  return true;
}

bool RollbackActiveTransaction(SessionState* state, std::string* error) {
  if (!state->active_transaction.valid()) {
    state->txn_id = 0;
    return true;
  }
  auto result = state->transaction_manager.Rollback(state->active_transaction, CurrentUnixEpochMillis());
  if (!result.ok()) {
    *error = DiagnosticText(result.diagnostic);
    return false;
  }
  state->active_transaction = {};
  state->txn_id = 0;
  return true;
}

bool CommitTransaction(SessionState* state, std::string* error) {
  if (!CommitActiveTransaction(state, error)) return false;
  return BeginTransaction(state, error);
}

bool RollbackTransaction(SessionState* state, std::string* error) {
  if (!RollbackActiveTransaction(state, error)) return false;
  return BeginTransaction(state, error);
}

bool SetAutocommit(SessionState* state, bool autocommit, std::string* error) {
  if (state->autocommit == autocommit) {
    return state->active_transaction.valid() ? true : BeginTransaction(state, error);
  }
  state->autocommit = autocommit;
  if (autocommit) return CommitTransaction(state, error);
  return BeginTransaction(state, error);
}

bool CreateSavepoint(SessionState* state, const std::string& name, std::string* error) {
  if (!state->active_transaction.valid() && !BeginTransaction(state, error)) return false;
  auto result = state->savepoints.Create(state->active_transaction, name, 0);
  if (!result.ok()) {
    *error = DiagnosticText(result.diagnostic);
    return false;
  }
  return true;
}

bool ReleaseSavepoint(SessionState* state, const std::string& name, std::string* error) {
  auto result = state->savepoints.Release(state->active_transaction, name);
  if (!result.ok()) {
    *error = DiagnosticText(result.diagnostic);
    return false;
  }
  return true;
}

bool RollbackToSavepoint(SessionState* state, const std::string& name, std::string* error) {
  auto result = state->savepoints.ApplyRollbackTo(state->active_transaction, name);
  if (!result.ok()) {
    *error = DiagnosticText(result.diagnostic);
    return false;
  }
  return true;
}

bool IsKnownSbwpMessage(std::uint8_t msg_type) {
  switch (msg_type) {
    case kStartup:
    case kQuery:
    case kParse:
    case kBind:
    case kDescribe:
    case kExecute:
    case kClose:
    case kSync:
    case kCancel:
    case kTerminate:
    case kCopyData:
    case kCopyDone:
    case kCopyFail:
    case kSblrExecute:
    case kSubscribe:
    case kUnsubscribe:
    case kStreamControl:
    case kTxnBegin:
    case kTxnCommit:
    case kTxnRollback:
    case kTxnSavepoint:
    case kTxnRelease:
    case kTxnRollbackTo:
    case kPing:
    case kSetOption:
    case kResetSession:
    case kReauth:
    case kTraceContext:
    case kLobClose:
    case kHeartbeat:
    case kExtension:
      return true;
    default:
      return false;
  }
}

std::uint64_t RequiredFeatureForMessage(std::uint8_t msg_type) {
  switch (msg_type) {
    case kCopyData:
    case kCopyDone:
    case kCopyFail:
      return kFeatureStreaming;
    case kSblrExecute:
      return kFeatureSblr;
    case kSubscribe:
    case kUnsubscribe:
      return kFeatureNotifications;
    case kStreamControl:
      return kFeatureCopyBackpressure;
    case kTxnSavepoint:
    case kTxnRelease:
    case kTxnRollbackTo:
      return kFeatureSavepoints;
    case kResetSession:
      return kFeatureSessionReset;
    case kReauth:
      return kFeatureReauth;
    case kTraceContext:
      return kFeatureTraceContext;
    case kLobClose:
      return kFeatureLobLocator;
    case kExtension:
      return ~0ull;
    default:
      return 0;
  }
}

bool FeatureNegotiated(const SessionState& state, std::uint64_t feature) {
  if (feature == 0) return true;
  if (feature == ~0ull) return false;
  if (!state.p1_payloads) return feature == kFeatureSavepoints;
  return (state.negotiated_features & feature) == feature;
}

class ClientIo {
 public:
  explicit ClientIo(int fd) : fd_(fd) {}
  ~ClientIo() {
    if (ssl_ != nullptr) {
      SSL_shutdown(ssl_);
      SSL_free(ssl_);
      ssl_ = nullptr;
    }
    if (ctx_ != nullptr) {
      SSL_CTX_free(ctx_);
      ctx_ = nullptr;
    }
#ifndef _WIN32
    if (fd_ >= 0) ::close(fd_);
#endif
  }

  bool StartTls(const std::string& cert_file, const std::string& key_file) {
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
    ctx_ = SSL_CTX_new(TLS_server_method());
    if (ctx_ == nullptr) return false;
    SSL_CTX_set_min_proto_version(ctx_, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx_, TLS1_3_VERSION);
    SSL_CTX_set_options(ctx_, SSL_OP_NO_COMPRESSION);
    if (SSL_CTX_use_certificate_chain_file(ctx_, cert_file.c_str()) != 1) return false;
    if (SSL_CTX_use_PrivateKey_file(ctx_, key_file.c_str(), SSL_FILETYPE_PEM) != 1) return false;
    if (SSL_CTX_check_private_key(ctx_) != 1) return false;
    ssl_ = SSL_new(ctx_);
    if (ssl_ == nullptr) return false;
    SSL_set_fd(ssl_, fd_);
    return SSL_accept(ssl_) == 1;
  }

  bool ReadExact(std::uint8_t* data, std::size_t size) {
    std::size_t got = 0;
    while (got < size) {
      int rc = 0;
      if (ssl_ != nullptr) {
        rc = SSL_read(ssl_, data + got, static_cast<int>(size - got));
      } else {
#ifndef _WIN32
        const ssize_t raw = ::read(fd_, data + got, size - got);
        rc = raw > 0 ? static_cast<int>(raw) : static_cast<int>(raw);
#else
        return false;
#endif
      }
      if (rc > 0) {
        got += static_cast<std::size_t>(rc);
        continue;
      }
#ifndef _WIN32
      if (ssl_ == nullptr && rc < 0 && errno == EINTR) continue;
#endif
      return false;
    }
    return true;
  }

  bool WriteAll(const std::uint8_t* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
      int rc = 0;
      if (ssl_ != nullptr) {
        rc = SSL_write(ssl_, data + written, static_cast<int>(size - written));
      } else {
#ifndef _WIN32
        const ssize_t raw = ::write(fd_, data + written, size - written);
        rc = raw > 0 ? static_cast<int>(raw) : static_cast<int>(raw);
#else
        return false;
#endif
      }
      if (rc > 0) {
        written += static_cast<std::size_t>(rc);
        continue;
      }
#ifndef _WIN32
      if (ssl_ == nullptr && rc < 0 && errno == EINTR) continue;
#endif
      return false;
    }
    return true;
  }

 private:
  int fd_{-1};
  SSL_CTX* ctx_{nullptr};
  SSL* ssl_{nullptr};
};

bool ReadFrame(ClientIo* io, Frame* frame) {
  std::array<std::uint8_t, kSbwpHeaderSize> header{};
  if (!io->ReadExact(header.data(), header.size())) return false;
  if (header[0] != 'S' || header[1] != 'B' || header[2] != 'W' || header[3] != 'P') return false;
  if (header[4] != kSbwpMajor || header[5] != kSbwpMinor) return false;
  frame->header.msg_type = header[6];
  frame->header.flags = header[7];
  frame->header.length = static_cast<std::uint32_t>(header[8]) |
                         (static_cast<std::uint32_t>(header[9]) << 8u) |
                         (static_cast<std::uint32_t>(header[10]) << 16u) |
                         (static_cast<std::uint32_t>(header[11]) << 24u);
  if (frame->header.length > kMaxPayloadBytes) return false;
  frame->header.sequence = static_cast<std::uint32_t>(header[12]) |
                           (static_cast<std::uint32_t>(header[13]) << 8u) |
                           (static_cast<std::uint32_t>(header[14]) << 16u) |
                           (static_cast<std::uint32_t>(header[15]) << 24u);
  std::copy(header.begin() + 16, header.begin() + 32, frame->header.attachment_id.begin());
  frame->header.txn_id = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    frame->header.txn_id |= static_cast<std::uint64_t>(header[32 + static_cast<std::size_t>(shift / 8)]) << shift;
  }
  frame->payload.assign(frame->header.length, 0);
  return frame->payload.empty() || io->ReadExact(frame->payload.data(), frame->payload.size());
}

bool SendFrame(ClientIo* io, SessionState* state, std::uint8_t msg_type, const std::vector<std::uint8_t>& payload) {
  std::vector<std::uint8_t> out;
  out.reserve(kSbwpHeaderSize + payload.size());
  out.push_back('S');
  out.push_back('B');
  out.push_back('W');
  out.push_back('P');
  out.push_back(kSbwpMajor);
  out.push_back(kSbwpMinor);
  out.push_back(msg_type);
  out.push_back(0);
  PutU32(&out, static_cast<std::uint32_t>(payload.size()));
  PutU32(&out, state->server_sequence++);
  out.insert(out.end(), state->attachment_id.begin(), state->attachment_id.end());
  PutU64(&out, state->txn_id);
  out.insert(out.end(), payload.begin(), payload.end());
  return io->WriteAll(out.data(), out.size());
}

std::vector<std::uint8_t> ReadyPayload(const SessionState& state, ReadyReason reason) {
  if (state.p1_payloads) {
    std::vector<std::uint8_t> out;
    PutUuid(&out, state.session_uuid);
    PutUuid(&out, state.attachment_id);
    PutZeroUuid(&out);
    PutU64(&out, state.txn_id);
    out.push_back(state.txn_id == 0 ? 0x52 : 0x54);
    out.push_back(static_cast<std::uint8_t>(reason));
    PutU16(&out, state.selected_protocol_version);
    PutU64(&out, state.negotiated_features);
    PutU32(&out, 0);
    PutU32(&out, 0);
    return out;
  }
  std::vector<std::uint8_t> out;
  out.reserve(20);
  out.push_back(state.txn_id == 0 ? 0 : 1);
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  PutU64(&out, state.txn_id);
  PutU64(&out, state.txn_id == 0 ? 0 : 1);
  return out;
}

std::vector<std::uint8_t> AuthOkPayload(const SessionState& state) {
  std::vector<std::uint8_t> out;
  out.insert(out.end(), state.attachment_id.begin(), state.attachment_id.end());
  PutU32(&out, 0);
  return out;
}

std::vector<std::uint8_t> CommandCompletePayload(std::uint64_t rows, std::string_view tag) {
  std::vector<std::uint8_t> out;
  out.reserve(21 + tag.size());
  out.push_back(1);
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  PutU64(&out, rows);
  PutU64(&out, 0);
  out.insert(out.end(), tag.begin(), tag.end());
  out.push_back(0);
  return out;
}

void PutCanonicalTypeRef(std::vector<std::uint8_t>* out, std::uint32_t oid) {
  std::uint16_t family = oid == 0 ? 0 : 8;
  std::uint16_t code = oid == 0 ? 0 : 1;
  if (oid == kOidInt8) {
    family = 2;
    code = 4;
  }
  PutU16(out, family);
  PutU16(out, code);
  PutU16(out, family == 0 ? 0 : 1);
  PutU16(out, 0);
  PutZeroUuid(out);
  PutZeroUuid(out);
  PutZeroUuid(out);
  PutU32(out, family == 0 ? 0 : 1);
  PutU32(out, 0);
  PutU64(out, 0);
  PutU64(out, 0);
  PutU32(out, 0);
  PutU32(out, 0);
  PutU64(out, 0);
  PutU64(out, 0);
  PutZeroUuid(out);
  PutZeroUuid(out);
  PutU16(out, 0);
  PutU16(out, 0);
  PutU16(out, 0);
  PutU16(out, 0);
}

std::vector<std::uint8_t> ParameterDescriptionPayload(const PreparedStatement& statement,
                                                      bool p1_payloads,
                                                      std::uint64_t negotiated_features) {
  if (p1_payloads) {
    std::vector<std::uint8_t> out;
    PutU16(&out, 1);
    out.push_back(0);
    out.push_back(1);
    PutZeroUuid(&out);
    PutZeroUuid(&out);
    PutHash256Zero(&out);
    PutU32(&out, static_cast<std::uint32_t>(statement.param_types.size()));
    for (std::size_t i = 0; i < statement.param_types.size(); ++i) {
      PutU32(&out, static_cast<std::uint32_t>(i + 1));
      out.push_back(0);
      out.push_back(0);
      out.push_back(2);
      out.push_back(0);
      std::uint64_t parameter_flags = FeatureBit(1) | FeatureBit(2);
      if ((negotiated_features & kFeatureArrayBind) != 0) parameter_flags |= FeatureBit(3);
      PutU64(&out, parameter_flags);
      PutU64(&out, FeatureBit(10));
      PutCanonicalTypeRef(&out, statement.param_types[i]);
      out.push_back(0);
      out.push_back(0);
      out.push_back(0);
      out.push_back(0);
      PutAbsentNullableText(&out);
    }
    return out;
  }
  std::vector<std::uint8_t> out;
  const std::uint16_t count = static_cast<std::uint16_t>(
      std::min<std::size_t>(statement.param_types.size(), UINT16_MAX));
  PutU16(&out, count);
  PutU16(&out, 0);
  for (std::uint16_t i = 0; i < count; ++i) {
    PutU32(&out, statement.param_types[i]);
  }
  return out;
}

std::vector<std::uint8_t> ErrorPayload(std::string_view sqlstate,
                                       std::string_view message,
                                       std::string_view detail = {}) {
  std::vector<std::uint8_t> out;
  out.reserve(32 + sqlstate.size() + message.size() + detail.size());
  auto field = [&](char code, std::string_view value) {
    if (value.empty()) return;
    out.push_back(static_cast<std::uint8_t>(code));
    out.insert(out.end(), value.begin(), value.end());
    out.push_back(0);
  };
  field('S', "ERROR");
  field('C', sqlstate);
  field('M', message);
  field('D', detail);
  out.push_back(0);
  return out;
}

bool SendError(ClientIo* io,
               SessionState* state,
               std::string_view sqlstate,
               std::string_view message,
               std::string_view detail = {}) {
  return SendFrame(io, state, kError, ErrorPayload(sqlstate, message, detail));
}

bool SendReady(ClientIo* io, SessionState* state, ReadyReason reason = ReadyReason::kCommandComplete) {
  std::string error;
  if (!BeginTransaction(state, &error)) {
    (void)SendError(io, state, "25001", "could not open replacement MGA transaction", error);
    return false;
  }
  return SendFrame(io, state, kReady, ReadyPayload(*state, reason));
}

void PutParameterStatusKv(std::vector<std::uint8_t>* out,
                          std::string_view key,
                          std::string_view value) {
  PutLpStr(out, key);
  out->push_back(0x01);
  out->push_back(0);
  out->push_back(0);
  PutU32(out, static_cast<std::uint32_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

std::vector<std::uint8_t> ParameterStatusPayload(
    const std::vector<std::pair<std::string, std::string>>& values) {
  std::vector<std::uint8_t> out;
  PutU32(&out, static_cast<std::uint32_t>(values.size()));
  for (const auto& [key, value] : values) PutParameterStatusKv(&out, key, value);
  return out;
}

bool SendParameterStatus(ClientIo* io,
                         SessionState* state,
                         const std::vector<std::pair<std::string, std::string>>& values) {
  if (!state->p1_payloads || values.empty()) return true;
  return SendFrame(io, state, kParameterStatus, ParameterStatusPayload(values));
}

std::vector<std::uint8_t> ServerInfoPayload(const SessionState& state) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, state.session_uuid);
  PutZeroUuid(&out);
  PutZeroUuid(&out);
  PutLpStr(&out, "ScratchBird SBSQL worker");
  PutLpStr(&out, "public-alpha");
  PutLpStr(&out, "native");
  PutU64(&out, 1);
  PutU64(&out, kServerSupportedFeatureMask);
  PutU64(&out, 1);
  return out;
}

bool SendServerInfo(ClientIo* io, SessionState* state) {
  if (!state->p1_payloads) return true;
  return SendFrame(io, state, kServerInfo, ServerInfoPayload(*state));
}

std::vector<std::pair<std::string, std::string>> StartupParameterStatuses(
    const SessionState& state) {
  std::vector<std::pair<std::string, std::string>> values;
  values.push_back({"protocol.selected_version",
                    state.selected_protocol_version == kSbwpVersionCurrent ? "1.1" : "1.0"});
  values.push_back({"protocol.negotiated_features", std::to_string(state.negotiated_features)});
  for (const auto& key : {"client_encoding", "charset", "timezone", "time_zone",
                          "application_name", "default_schema", "default_catalog"}) {
    const auto found = state.session_parameters.find(key);
    if (found != state.session_parameters.end()) values.push_back({key, found->second});
  }
  return values;
}

bool SendFeatureNotNegotiated(ClientIo* io, SessionState* state, std::uint8_t msg_type) {
  return SendError(io,
                   state,
                   "08P01",
                   "SBWP.FEATURE.NOT_NEGOTIATED",
                   "frame 0x" + std::to_string(msg_type) +
                       " requires a feature that was not negotiated");
}

bool SendUnsupportedFeature(ClientIo* io, SessionState* state, std::string_view feature_name) {
  return SendError(io,
                   state,
                   "0A000",
                   "FEATURE.NOT_IMPLEMENTED_RELEASE_BLOCKING",
                   std::string(feature_name) + " is fail-closed before side effects in this route");
}

std::array<std::uint8_t, 16> PayloadUuidOrGenerated(const std::vector<std::uint8_t>& payload) {
  std::array<std::uint8_t, 16> uuid{};
  if (payload.size() >= uuid.size()) {
    std::copy(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(uuid.size()), uuid.begin());
    return uuid;
  }
  for (std::size_t i = 0; i < uuid.size(); ++i) uuid[i] = static_cast<std::uint8_t>(0xa0u + i);
  return uuid;
}

std::vector<std::uint8_t> CancelAckPayload(const std::array<std::uint8_t, 16>& cancel_uuid) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, cancel_uuid);
  out.push_back(1);
  return out;
}

std::vector<std::uint8_t> CancelledPayload(const std::array<std::uint8_t, 16>& cancel_uuid,
                                           std::string_view outcome_class,
                                           std::string_view sqlstate,
                                           std::string_view diagnostic_code,
                                           std::string_view transaction_effect,
                                           std::string_view retryability) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, cancel_uuid);
  PutLpStr(&out, outcome_class);
  PutLpStr(&out, sqlstate);
  PutLpStr(&out, diagnostic_code);
  PutLpStr(&out, transaction_effect);
  PutLpStr(&out, retryability);
  return out;
}

bool HandleCancel(ClientIo* io, SessionState* state, const Frame& frame) {
  const auto cancel_uuid = PayloadUuidOrGenerated(frame.payload);
  if (!SendFrame(io, state, kCancelAck, CancelAckPayload(cancel_uuid))) return false;
  const auto payload = CancelledPayload(cancel_uuid,
                                        "cancel_target_not_found",
                                        "57014",
                                        "EXECUTION.CANCEL_TARGET_NOT_FOUND",
                                        "no_known_state_change",
                                        "no");
  return SendFrame(io, state, kCancelled, payload) &&
         SendReady(io, state, ReadyReason::kCancelOutcome);
}

bool HandleResetSession(ClientIo* io, SessionState* state, const Frame& frame) {
  if (!FeatureNegotiated(*state, kFeatureSessionReset)) {
    return SendFeatureNotNegotiated(io, state, frame.header.msg_type);
  }
  const bool rollback_open_transaction = frame.payload.size() > 17 && frame.payload[17] != 0;
  if (state->active_transaction.valid() && !rollback_open_transaction) {
    return SendError(io,
                     state,
                     "25001",
                     "NATIVE_WIRE.RESET_REFUSED_OPEN_TRANSACTION",
                     "reset_session requires rollback_open_transaction when a transaction is open") &&
           SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  std::string error;
  if (rollback_open_transaction && !RollbackActiveTransaction(state, &error)) {
    return SendError(io, state, "08006", "SESSION.RESET_TIMEOUT", error) &&
           SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  state->statements.clear();
  state->bound_sql.clear();
  state->bound_param_types.clear();
  state->savepoints = SavepointStack{};
  state->session_parameters.erase("traceparent");
  state->session_parameters.erase("tracestate");
  return SendParameterStatus(io, state, {{"session.reset", "complete"}}) &&
         SendReady(io, state, ReadyReason::kResetComplete);
}

bool HandleReauth(ClientIo* io, SessionState* state, const Frame& frame) {
  if (!FeatureNegotiated(*state, kFeatureReauth)) {
    return SendFeatureNotNegotiated(io, state, frame.header.msg_type);
  }
  if (frame.payload.empty()) {
    return SendError(io,
                     state,
                     "28000",
                     "SECURITY.REAUTH_TIMEOUT",
                     "reauth requires a non-empty engine-verifiable auth payload") &&
           SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  return SendParameterStatus(io, state, {{"security.generation", "refreshed"}}) &&
         SendReady(io, state, ReadyReason::kReauthComplete);
}

bool ValidTraceparent(std::string_view traceparent) {
  if (traceparent.empty()) return true;
  return traceparent.size() == 55 && traceparent[2] == '-' && traceparent[35] == '-' &&
         traceparent[52] == '-';
}

bool HandleTraceContext(ClientIo* io, SessionState* state, const Frame& frame) {
  if (!FeatureNegotiated(*state, kFeatureTraceContext)) {
    return SendFeatureNotNegotiated(io, state, frame.header.msg_type);
  }
  std::size_t off = 16;
  std::string traceparent;
  std::string tracestate;
  if (frame.payload.size() < 18 ||
      !ReadLpStr(frame.payload, &off, &traceparent) ||
      !ReadLpStr(frame.payload, &off, &tracestate) ||
      off + 2 > frame.payload.size() ||
      !ValidTraceparent(traceparent)) {
    return SendError(io,
                     state,
                     "08P01",
                     "NATIVE_WIRE.TRACE_CONTEXT_INVALID",
                     "trace context failed W3C shape or payload validation") &&
           SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  state->session_parameters["traceparent"] = traceparent;
  state->session_parameters["tracestate"] = tracestate.empty() ? "present" : "redacted";
  return SendParameterStatus(io, state, {{"traceparent", traceparent.empty() ? "absent" : "present"}}) &&
         SendReady(io, state, ReadyReason::kStateChange);
}

bool HandleSubscription(ClientIo* io, SessionState* state, bool subscribe) {
  if (!FeatureNegotiated(*state, kFeatureNotifications)) {
    return SendFeatureNotNegotiated(io, state, subscribe ? kSubscribe : kUnsubscribe);
  }
  if (!SendFrame(io,
                 state,
                 kCommandComplete,
                 CommandCompletePayload(0, subscribe ? "SUBSCRIBE" : "UNSUBSCRIBE"))) {
    return false;
  }
  if (subscribe) {
    std::vector<std::uint8_t> payload;
    PutLpStr(&payload, "session");
    PutLpStr(&payload, "subscription_active");
    PutU32(&payload, 0);
    if (!SendFrame(io, state, kNotification, payload)) return false;
  }
  return SendReady(io, state, ReadyReason::kCommandComplete);
}

std::vector<std::uint8_t> RowDescriptionPayload(const std::vector<std::string>& column_names,
                                                bool p1_payloads) {
  if (p1_payloads) {
    std::vector<std::uint8_t> out;
    PutU16(&out, 1);
    out.push_back(0);
    out.push_back(1);
    PutU32(&out, static_cast<std::uint32_t>(column_names.size()));
    PutZeroUuid(&out);
    PutZeroUuid(&out);
    PutHash256Zero(&out);
    std::uint32_t ordinal = 1;
    for (const auto& name : column_names) {
      PutU32(&out, ordinal++);
      out.push_back(0);
      out.push_back(1);
      out.push_back(1);
      out.push_back(0);
      PutU64(&out, FeatureBit(0) | FeatureBit(10));
      PutCanonicalTypeRef(&out, 0);
      PutZeroUuid(&out);
      PutZeroUuid(&out);
      PutZeroUuid(&out);
      PutU32(&out, 0);
      out.push_back(0);
      out.push_back(0);
      PutU16(&out, 0);
      PutNullableText(&out, name);
    }
    return out;
  }
  std::vector<std::uint8_t> out;
  PutU16(&out, static_cast<std::uint16_t>(std::min<std::size_t>(column_names.size(), UINT16_MAX)));
  PutU16(&out, 0);
  std::uint16_t ordinal = 0;
  for (const auto& name : column_names) {
    PutU32(&out, static_cast<std::uint32_t>(name.size()));
    out.insert(out.end(), name.begin(), name.end());
    PutU32(&out, 0);
    PutU16(&out, ordinal++);
    PutU32(&out, 0);
    PutI16(&out, -1);
    PutI32(&out, -1);
    out.push_back(0);
    out.push_back(1);
    PutU16(&out, 0);
  }
  return out;
}

std::vector<std::uint8_t> DataRowPayload(const std::vector<std::optional<std::string>>& values) {
  std::vector<std::uint8_t> out;
  PutU16(&out, static_cast<std::uint16_t>(std::min<std::size_t>(values.size(), UINT16_MAX)));
  const std::size_t null_bytes = (values.size() + 7u) / 8u;
  PutU16(&out, static_cast<std::uint16_t>(null_bytes));
  const std::size_t bitmap_offset = out.size();
  out.resize(out.size() + null_bytes, 0);
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (!values[i].has_value()) {
      out[bitmap_offset + (i / 8u)] |= static_cast<std::uint8_t>(1u << (i % 8u));
      continue;
    }
    const auto& value = *values[i];
    PutI32(&out, static_cast<std::int32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
  }
  return out;
}

bool SendRows(ClientIo* io,
              SessionState* state,
              const std::vector<std::string>& columns,
              const std::vector<std::vector<std::optional<std::string>>>& rows,
              std::string_view tag) {
  if (!SendFrame(io, state, kRowDescription, RowDescriptionPayload(columns, state->p1_payloads))) return false;
  for (const auto& row : rows) {
    if (!SendFrame(io, state, kDataRow, DataRowPayload(row))) return false;
  }
  return SendFrame(io,
                   state,
                   kCommandComplete,
                   CommandCompletePayload(rows.size(), tag.empty() ? "SELECT " + std::to_string(rows.size()) : tag));
}

bool ExecuteSql(ClientIo* io, SessionState* state, std::string_view raw_sql) {
  const std::string sql = StripSqlTerminator(std::string(raw_sql));
  const std::string lowered = Lower(sql);
  if (lowered.empty()) {
    return SendFrame(io, state, kParseComplete, {});
  }
  if (StartsWithWord(lowered, "show") && lowered.find("transaction isolation") != std::string::npos) {
    return SendRows(io, state, {"transaction_isolation"}, {{{"READ COMMITTED"}}}, "SHOW");
  }
  if (StartsWithWord(lowered, "begin") || StartsWithWord(lowered, "start transaction")) {
    std::string error;
    if (!BeginTransaction(state, &error)) {
      return SendError(io, state, "25001", "could not begin transaction", error);
    }
    return SendFrame(io, state, kCommandComplete, CommandCompletePayload(0, "BEGIN"));
  }
  if (StartsWithWord(lowered, "commit")) {
    std::string error;
    if (!CommitTransaction(state, &error)) {
      return SendError(io, state, "40000", "could not commit transaction", error);
    }
    return SendFrame(io, state, kCommandComplete, CommandCompletePayload(0, "COMMIT"));
  }
  if (StartsWithWord(lowered, "rollback") && !StartsWithWord(lowered, "rollback to")) {
    std::string error;
    if (!RollbackTransaction(state, &error)) {
      return SendError(io, state, "40000", "could not roll back transaction", error);
    }
    return SendFrame(io, state, kCommandComplete, CommandCompletePayload(0, "ROLLBACK"));
  }
  if (StartsWithWord(lowered, "savepoint")) {
    std::string error;
    if (!CreateSavepoint(state, FirstTokenAfter(sql, "savepoint"), &error)) {
      return SendError(io, state, "3B000", "savepoint operation failed", error);
    }
    return SendFrame(io, state, kCommandComplete, CommandCompletePayload(0, "SAVEPOINT"));
  }
  if (StartsWithWord(lowered, "release")) {
    std::string error;
    if (!ReleaseSavepoint(state, FirstTokenAfter(sql, "release"), &error)) {
      return SendError(io, state, "3B000", "savepoint operation failed", error);
    }
    return SendFrame(io, state, kCommandComplete, CommandCompletePayload(0, "RELEASE"));
  }
  if (StartsWithWord(lowered, "rollback to")) {
    std::string error;
    if (!RollbackToSavepoint(state, FirstTokenAfter(sql, "rollback to"), &error)) {
      return SendError(io, state, "3B000", "savepoint operation failed", error);
    }
    return SendFrame(io, state, kCommandComplete, CommandCompletePayload(0, "ROLLBACK"));
  }

  return SendError(io,
                   state,
                   "0A000",
                   "native SQL execution requires the SBLR engine route",
                   "transaction control uses MGA; SQL text is not executed inside the parser worker");
}

std::string Env(const char* name);

bool ServeClient(int fd, bool tls_required, const std::string& cert_file, const std::string& key_file) {
  ClientIo io(fd);
  if (tls_required && !io.StartTls(cert_file, key_file)) return false;

  SessionState state;
  for (std::size_t i = 0; i < state.attachment_id.size(); ++i) {
    state.attachment_id[i] = static_cast<std::uint8_t>(0x30u + i);
  }
  state.session_uuid = state.attachment_id;

  for (;;) {
    Frame frame;
    if (!ReadFrame(&io, &frame)) return true;
    if ((frame.header.flags & ~kFrameFlagKnownMask) != 0 ||
        (frame.header.flags & kFrameFlagPartial) != 0 ||
        ((frame.header.flags & kFrameFlagCompressed) != 0 &&
         !FeatureNegotiated(state, kFeatureCompression))) {
      (void)SendError(&io,
                      &state,
                      "08P01",
                      (frame.header.flags & kFrameFlagCompressed) != 0
                          ? "SBWP.COMPRESSION.UNNEGOTIATED_FRAME"
                          : "SBWP.HEADER.RESERVED_BITS_SET",
                      "SBWP frame flags failed closed before payload dispatch");
      return false;
    }
    if (!IsKnownSbwpMessage(frame.header.msg_type)) {
      (void)SendError(&io,
                      &state,
                      "08P01",
                      "NATIVE_WIRE.PROTOCOL_FATAL",
                      "unknown SBWP frame type is not recoverable");
      return false;
    }
    const auto required_feature = RequiredFeatureForMessage(frame.header.msg_type);
    if (!FeatureNegotiated(state, required_feature)) {
      (void)SendFeatureNotNegotiated(&io, &state, frame.header.msg_type);
      return false;
    }
    switch (frame.header.msg_type) {
      case kStartup:
        {
        const auto startup = ParseStartupNegotiation(frame.payload);
        if (!startup.ok) {
          state.p1_payloads = startup.p1;
          (void)SendError(&io, &state, startup.sqlstate, startup.message, startup.detail);
          return false;
        }
        state.p1_payloads = startup.p1;
        state.selected_protocol_version = startup.selected_protocol_version;
        state.negotiated_features = startup.negotiated_features;
        state.session_parameters = startup.params;
        if (!SendFrame(&io, &state, kAuthOk, AuthOkPayload(state))) return false;
        if (!SendServerInfo(&io, &state)) return false;
        if (!SendParameterStatus(&io, &state, StartupParameterStatuses(state))) return false;
        if (!SendReady(&io, &state, ReadyReason::kStartup)) return false;
        break;
        }
      case kQuery: {
        const std::string sql = ParseQuerySql(frame.payload);
        if (!ExecuteSql(&io, &state, sql)) return false;
        if (!SendReady(&io, &state)) return false;
        break;
      }
      case kParse: {
        std::string name;
        auto prepared = ParsePreparedStatement(frame.payload, &name);
        if (prepared) {
          if (prepared->param_types.empty()) {
            const auto placeholders = CountPlaceholders(prepared->sql);
            prepared->param_types.assign(static_cast<std::size_t>(placeholders), kOidInt8);
          }
          state.statements[name] = *prepared;
          state.bound_sql = prepared->sql;
          state.bound_param_types = prepared->param_types;
        }
        if (!SendFrame(&io, &state, kParseComplete, {})) return false;
        break;
      }
      case kDescribe: {
        std::size_t off = 4;
        const std::string name = ReadSizedString(frame.payload, &off);
        PreparedStatement statement = state.statements[name];
        if (statement.sql.empty()) {
          statement.sql = state.bound_sql;
          statement.param_types = state.bound_param_types;
        }
        if (!SendFrame(&io,
                       &state,
                       kParameterDescription,
                       ParameterDescriptionPayload(statement,
                                                   state.p1_payloads,
                                                   state.negotiated_features))) return false;
        break;
      }
      case kBind: {
        if (state.p1_payloads && frame.payload.size() >= 4 && ReadU16(frame.payload, 0) == 1 &&
            frame.payload[3] != 0) {
          if (!FeatureNegotiated(state, kFeatureArrayBind)) {
            if (!SendFeatureNotNegotiated(&io, &state, frame.header.msg_type)) return false;
          } else if (!SendUnsupportedFeature(&io, &state, "array-bind execute")) {
            return false;
          }
          break;
        }
        const std::string name = ParseBindStatementName(frame.payload);
        const auto found = state.statements.find(name);
        if (found != state.statements.end()) {
          state.bound_sql = found->second.sql;
          state.bound_param_types = found->second.param_types;
        }
        break;
      }
      case kExecute:
        if (!ExecuteSql(&io, &state, state.bound_sql)) return false;
        break;
      case kSync:
        if (!SendReady(&io, &state)) return false;
        break;
      case kClose:
        if (!SendFrame(&io, &state, kCloseComplete, {})) return false;
        break;
      case kTxnBegin: {
        std::string error;
        if (!BeginTransaction(&state, &error)) {
          if (!SendError(&io, &state, "25001", "could not begin transaction", error)) return false;
        }
        if (!SendReady(&io, &state)) return false;
        break;
      }
      case kTxnCommit: {
        std::string error;
        if (!CommitTransaction(&state, &error)) {
          if (!SendError(&io, &state, "40000", "could not commit transaction", error)) return false;
        }
        if (!SendReady(&io, &state)) return false;
        break;
      }
      case kTxnRollback: {
        std::string error;
        if (!RollbackTransaction(&state, &error)) {
          if (!SendError(&io, &state, "40000", "could not roll back transaction", error)) return false;
        }
        if (!SendReady(&io, &state)) return false;
        break;
      }
      case kTxnSavepoint:
      case kTxnRelease:
      case kTxnRollbackTo: {
        std::size_t off = 0;
        const std::string name = ReadSizedString(frame.payload, &off);
        std::string error;
        bool ok = false;
        if (frame.header.msg_type == kTxnSavepoint) ok = CreateSavepoint(&state, name, &error);
        if (frame.header.msg_type == kTxnRelease) ok = ReleaseSavepoint(&state, name, &error);
        if (frame.header.msg_type == kTxnRollbackTo) ok = RollbackToSavepoint(&state, name, &error);
        if (!ok && !SendError(&io, &state, "3B000", "savepoint operation failed", error)) return false;
        if (!SendReady(&io, &state)) return false;
        break;
      }
      case kSetOption: {
        std::size_t off = 0;
        const std::string name = Lower(ReadSizedString(frame.payload, &off));
        const std::string value = Lower(ReadSizedString(frame.payload, &off));
        std::string error;
        if (name == "autocommit") {
          const bool enabled = value == "1" || value == "true" || value == "on" || value == "yes";
          if (!SetAutocommit(&state, enabled, &error)) {
            if (!SendError(&io, &state, "25000", "could not change autocommit", error)) return false;
          }
        }
        if (!SendReady(&io, &state)) return false;
        break;
      }
      case kPing:
        if (!SendFrame(&io, &state, kPong, frame.payload)) return false;
        break;
      case kCancel:
        if (!HandleCancel(&io, &state, frame)) return false;
        break;
      case kResetSession:
        if (!HandleResetSession(&io, &state, frame)) return false;
        break;
      case kReauth:
        if (!HandleReauth(&io, &state, frame)) return false;
        break;
      case kTraceContext:
        if (!HandleTraceContext(&io, &state, frame)) return false;
        break;
      case kSubscribe:
        if (!HandleSubscription(&io, &state, true)) return false;
        break;
      case kUnsubscribe:
        if (!HandleSubscription(&io, &state, false)) return false;
        break;
      case kCopyData:
      case kCopyDone:
      case kCopyFail:
      case kSblrExecute:
      case kStreamControl:
      case kLobClose:
        if (!SendUnsupportedFeature(&io, &state, "native extension frame") ||
            !SendReady(&io, &state, ReadyReason::kErrorRecovered)) return false;
        break;
      case kTerminate:
        return true;
      default:
        if (!SendError(&io, &state, "08P01", "unsupported SBWP frame")) return false;
        return true;
    }
  }
}

std::uint64_t DecodeHandoffConnectionId(const std::vector<std::uint8_t>& payload,
                                        std::uint64_t fallback) {
  if (payload.size() < 8) return fallback;
  return ReadU64(payload, 0);
}

bool DecodeHandoffTlsActive(const std::vector<std::uint8_t>& payload) {
  constexpr std::size_t tls_active_offset = 8 + 16 + 48 + 2;
  return payload.size() > tls_active_offset && payload[tls_active_offset] != 0;
}

std::string Env(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string(value);
}

std::uint32_t EnvU32(const char* name, std::uint32_t fallback) {
  const std::string value = Env(name);
  if (value.empty()) return fallback;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
  if (end == value.c_str() || *end != '\0') return fallback;
  return static_cast<std::uint32_t>(parsed);
}

bool SendHello(int control_fd) {
  scratchbird::listener::ParserHelloPayload hello;
  hello.protocol = Env("SB_PROTOCOL_FAMILY").empty() ? "native" : Env("SB_PROTOCOL_FAMILY");
  hello.pid = static_cast<std::uint32_t>(::getpid());
  hello.worker_id = EnvU32("SB_PARSER_WORKER_NUMERIC_ID", 1);
  hello.dialect_protocol_version = 1;
  hello.parser_api_major = EnvU32("SB_PARSER_API_MAJOR", 1);
  hello.profile_id = Env("SB_PARSER_PROFILE_ID").empty() ? "native" : Env("SB_PARSER_PROFILE_ID");
  hello.bundle_contract_id = Env("SB_PARSER_BUNDLE_CONTRACT_ID").empty()
                                 ? "bundle.default@1"
                                 : Env("SB_PARSER_BUNDLE_CONTRACT_ID");

  scratchbird::listener::ListenerControlFrame frame;
  frame.opcode = scratchbird::listener::ListenerControlOpcode::kHello;
  frame.sequence = hello.worker_id;
  frame.payload = scratchbird::listener::EncodeHelloPayload(hello);
  return scratchbird::listener::SendControlFrame(control_fd, frame);
}

int RunWorker() {
#ifdef _WIN32
  std::cerr << "sbp_native listener-worker mode is not attached on Windows.\n";
  return 1;
#else
  const int control_fd = static_cast<int>(EnvU32("SB_LISTENER_CONTROL_FD", 0));
  if (control_fd <= 0) {
    std::cerr << "SB_LISTENER_CONTROL_FD is required.\n";
    return 1;
  }
  if (!SendHello(control_fd)) return 1;

  scratchbird::listener::ListenerControlDecodeResult decoded;
  int received_fd = -1;
  if (!scratchbird::listener::ReadControlFrame(control_fd, &decoded, &received_fd, 30000) ||
      decoded.frame.opcode != scratchbird::listener::ListenerControlOpcode::kHelloAck) {
    if (received_fd >= 0) ::close(received_fd);
    return 1;
  }
  if (received_fd >= 0) ::close(received_fd);
  auto hello_ack = scratchbird::listener::DecodeHelloAckPayload(decoded.frame.payload, &decoded.messages);
  if (!hello_ack || !hello_ack->accepted) return 1;

  for (;;) {
    scratchbird::listener::ListenerControlDecodeResult inbound;
    int client_fd = -1;
    if (!scratchbird::listener::ReadControlFrame(control_fd, &inbound, &client_fd, 300000)) {
      if (client_fd >= 0) ::close(client_fd);
      return 0;
    }
    if (inbound.frame.opcode == scratchbird::listener::ListenerControlOpcode::kRecycle ||
        inbound.frame.opcode == scratchbird::listener::ListenerControlOpcode::kShutdown) {
      if (client_fd >= 0) ::close(client_fd);
      return 0;
    }
    if (inbound.frame.opcode == scratchbird::listener::ListenerControlOpcode::kHealthCheck) {
      scratchbird::listener::ListenerControlFrame response;
      response.opcode = scratchbird::listener::ListenerControlOpcode::kHealthReport;
      response.sequence = inbound.frame.sequence;
      response.payload = scratchbird::listener::EncodeHealthReportPayload(
          scratchbird::listener::HealthReportPayload{inbound.frame.sequence, 0, 0});
      scratchbird::listener::SendControlFrame(control_fd, response);
      if (client_fd >= 0) ::close(client_fd);
      continue;
    }
    if (inbound.frame.opcode != scratchbird::listener::ListenerControlOpcode::kHandoffSocket) {
      if (client_fd >= 0) ::close(client_fd);
      continue;
    }
    const std::uint64_t connection_id = DecodeHandoffConnectionId(inbound.frame.payload, inbound.frame.sequence);
    scratchbird::listener::HandoffAckPayload ack;
    ack.connection_id_echo = connection_id;
    ack.accepted = client_fd >= 0;
    ack.reason = ack.accepted ? "" : "client_fd_missing";
    scratchbird::listener::ListenerControlFrame response;
    response.opcode = scratchbird::listener::ListenerControlOpcode::kHandoffAck;
    response.sequence = inbound.frame.sequence;
    response.payload = scratchbird::listener::EncodeHandoffAckPayload(ack);
    scratchbird::listener::SendControlFrame(control_fd, response);
    if (client_fd < 0) continue;

    const bool tls_required = Env("SB_TLS_REQUIRED") == "1" || DecodeHandoffTlsActive(inbound.frame.payload);
    const bool ok = ServeClient(client_fd, tls_required, Env("SB_TLS_CERT_FILE"), Env("SB_TLS_KEY_FILE"));
    return ok ? 0 : 1;
  }
#endif
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string_view(argv[1]) == "--listener-worker") {
    return RunWorker();
  }
  std::cout << "sbp_native requires --listener-worker under sb_listener control.\n";
  return 0;
}
