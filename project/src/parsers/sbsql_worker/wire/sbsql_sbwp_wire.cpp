// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "wire/sbsql_test_wire.hpp"

#include "embedded/embedded_engine_client.hpp"
#include "datatype_wire_metadata.hpp"
#include "ipc/sbps_client.hpp"
#include "rendering/rendering.hpp"
#include "uuid.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#ifdef UuidToString
#undef UuidToString
#endif
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace scratchbird::parser::sbsql {
namespace {
namespace datatypes = scratchbird::core::datatypes;
constexpr std::uint32_t kQueryFlagAutocommit = 0x40;
constexpr std::uint32_t kQueryFlagScriptIngest = 0x80;
constexpr std::uint32_t kQueryFlagScriptSizeHint = 0x100;
constexpr std::uint32_t kExecuteFlagAutocommit = 0x01;
constexpr std::uint8_t kTxnFinalityPayloadVersion = 1;
constexpr std::uint16_t kTxnCommitFlagHasIdempotencyKey = 0x0001;
constexpr std::uint16_t kTxnCommitFlagCallerAcknowledgedRetryBoundary = 0x0002;
constexpr std::uint16_t kTxnCommitFlagStatementHasSideEffects = 0x0004;
constexpr std::uint16_t kTxnFinalityFlagEngineKnown = 0x0001;
constexpr std::uint16_t kTxnFinalityFlagRetryAllowed = 0x0002;
constexpr std::uint16_t kTxnFinalityFlagRetryRefused = 0x0004;
constexpr std::uint16_t kTxnFinalityFlagSideEffectRetryRefused = 0x0008;
constexpr std::uint16_t kTxnFinalityFlagSameIdempotencyKeyReplayable = 0x0010;
constexpr std::uint16_t kTxnFinalityFlagPostInventorySecondaryFailure = 0x0020;


namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::size_t kSbwpHeaderSize = 40;
constexpr std::size_t kMaxPreparedStatementsPerSession = 256;
constexpr std::size_t kMaxPreparedStatementSqlBytesPerSession = 4u * 1024u * 1024u;
constexpr std::size_t kMaxBoundPortalsPerSession = 256;
constexpr std::size_t kMaxBoundPortalSqlBytesPerSession = 4u * 1024u * 1024u;
constexpr std::uint8_t kSbwpMajor = 1;
constexpr std::uint8_t kSbwpMinor = 1;
constexpr std::uint16_t kSbwpVersionMin = 0x0100;
constexpr std::uint16_t kSbwpVersionCurrent = 0x0101;
constexpr std::uint32_t kMaxPayloadBytes = 64u * 1024u * 1024u;
constexpr std::uint8_t kFrameFlagCompressed = 1u << 0;
constexpr std::uint8_t kFrameFlagPartial = 1u << 1;
constexpr std::uint8_t kFrameFlagFinal = 1u << 2;
constexpr std::uint8_t kFrameFlagKnownMask =
    kFrameFlagCompressed | kFrameFlagPartial | kFrameFlagFinal;

constexpr std::uint32_t kOidBool = 16;
constexpr std::uint32_t kOidInt8 = 20;
constexpr std::uint32_t kOidInt2 = 21;
constexpr std::uint32_t kOidInt4 = 23;
constexpr std::uint32_t kOidText = 25;
constexpr std::uint32_t kOidFloat4 = 700;
constexpr std::uint32_t kOidFloat8 = 701;
constexpr std::uint32_t kOidVarchar = 1043;
constexpr std::uint32_t kOidDate = 1082;
constexpr std::uint32_t kOidTime = 1083;
constexpr std::uint32_t kOidTimestamp = 1114;
constexpr std::uint32_t kOidTimestamptz = 1184;
constexpr std::uint32_t kOidNumeric = 1700;
constexpr std::uint32_t kOidUuid = 2950;

enum Msg : std::uint8_t {
  kStartup = 0x01,
  kAuthResponse = 0x02,
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

  kAuthRequest = 0x40,
  kAuthOk = 0x41,
  kReady = 0x43,
  kRowDescription = 0x44,
  kDataRow = 0x45,
  kCommandComplete = 0x46,
  kError = 0x48,
  kParseComplete = 0x4a,
  kBindComplete = 0x4b,
  kCloseComplete = 0x4c,
  kParameterStatus = 0x4f,
  kParameterDescription = 0x50,
  kCopyInResponse = 0x51,
  kNotification = 0x54,
  kTxnStatus = 0x5c,
  kPong = 0x5d,
  kQueryProgress = 0x60,
  kServerInfo = 0x61,
  kStateNotification = 0x62,
  kCancelAck = 0x65,
  kCancelled = 0x66,
  kMultiResultBegin = 0x67,
  kMultiResultEnd = 0x68,
  kGeneratedKeys = 0x69,
  kOutParameters = 0x6a,
  kBatchResult = 0x6b,
  kPipelineStatus = 0x6c,
  kArrayBindStatus = 0x6d,
  kBulkRejectData = 0x6e,
  kLobLocator = 0x6f,
  kLobChunk = 0x70,
  kLobClose = 0x71,
  kCursorStatus = 0x72,
  kFailoverHint = 0x73,
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
constexpr std::uint8_t kCopyFormatCanonicalRowFieldsText = 0x00;
constexpr std::uint8_t kCopyFormatBinaryRowsetV1 = 0x01;
constexpr std::uint32_t kCopyDefaultWindowBytes = 1024u * 1024u;
constexpr std::uint32_t kCopyBinaryWindowBytes = 32u * 1024u * 1024u;
constexpr std::size_t kCopyExecuteRowsPerSblrEnvelope = 50000;
constexpr std::size_t kScriptInsertGroupFlushRows = kCopyExecuteRowsPerSblrEnvelope;
constexpr std::uint64_t kAutoCursorFetchRows = 1024;
constexpr std::uint64_t kAutoCursorFetchBytes = 4u * 1024u * 1024u;
constexpr std::uint32_t kQueryScriptMetadataMagic = 0x53514253u;  // "SBQS"
constexpr std::uint16_t kQueryScriptMetadataVersion = 1;
constexpr std::uint64_t kKnownCoreFeatureMask = (FeatureBit(25) - 1u);
constexpr std::uint64_t kP1OnlyFeatureMask =
    kFeatureMultiResult | kFeatureGeneratedKeys | kFeatureOutParameters |
    kFeatureArrayBind | kFeatureBulkRejects | kFeatureLobLocator | kFeatureCursors |
    kFeatureCopyBackpressure | kFeatureSessionReset | kFeatureReauth |
    kFeatureFailoverHints | kFeatureTraceContext;
constexpr std::uint64_t kServerSupportedFeatureMask =
    kFeatureStreaming | kFeatureSblr | kFeatureNotifications | kFeatureBatch | kFeaturePipeline |
    kFeatureBinaryCopy | kFeatureSavepoints | kFeatureMultiResult |
    kFeatureGeneratedKeys | kFeatureOutParameters | kFeatureArrayBind |
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
  struct InsertRowsetPlan {
    std::string target_name;
    std::string target_object_uuid;
    std::string server_prepared_statement_uuid;
    std::string server_operation_id;
    std::vector<std::string> column_names;
    std::vector<std::size_t> parameter_indexes;
  };
  std::optional<InsertRowsetPlan> insert_rowset_plan;
};

struct BoundPortal {
  std::string sql;
  std::vector<std::uint32_t> param_types;
  std::vector<std::optional<std::string>> param_values;
  std::vector<std::vector<std::optional<std::string>>> param_rows;
  std::optional<PreparedStatement::InsertRowsetPlan> insert_rowset_plan;
};

struct SbwpColumn {
  std::string name;
  std::uint32_t type_oid{kOidText};
  std::int16_t type_size{-1};
  std::int32_t type_modifier{-1};
  std::uint8_t format{0};
  bool nullable{true};
};

struct RowSet {
  std::vector<SbwpColumn> columns;
  std::vector<std::vector<std::optional<std::string>>> rows;
};

struct CopyImportRow {
  std::vector<std::pair<std::string, std::optional<std::string>>> fields;
};

struct NativeCopyPacket {
  std::vector<std::uint8_t> payload;
  std::vector<std::string> columns;
  std::vector<std::uint8_t> column_type_tags;
  std::uint64_t row_count{0};
};

struct CopyImportState {
  bool active{false};
  bool native_bulk_ingest{false};
  bool native_bulk_ingest_enabled{true};
  std::string sql;
  std::string target_object_uuid;
  std::string prepared_statement_uuid;
  std::string prepared_operation_id;
  std::uint64_t prepared_catalog_epoch{0};
  std::uint64_t prepared_security_policy_epoch{0};
  std::uint64_t prepared_grant_epoch{0};
  std::uint64_t prepared_descriptor_epoch{0};
  std::uint64_t prepared_localized_name_epoch{0};
  std::uint64_t prepared_language_resource_epoch{0};
  std::uint64_t prepared_message_resource_epoch{0};
  std::string format_family{"canonical_row_fields"};
  std::uint8_t copy_data_format{kCopyFormatCanonicalRowFieldsText};
  std::uint32_t window_bytes{kCopyDefaultWindowBytes};
  std::uint64_t source_size_bytes{0};
  std::uint64_t preallocation_bytes{0};
  std::uint64_t preallocation_factor_percent{82};
  std::string target_name;
  bool native_packet_descriptor_bound{false};
  std::string native_packet_descriptor_fingerprint;
  std::uint32_t native_packet_descriptor_column_count{0};
  bool aggregate_result_active{false};
  PipelineResult aggregate_result;
  std::size_t aggregate_chunk_count{0};
  std::vector<CopyImportRow> rows;
  std::vector<NativeCopyPacket> native_packets;
};

struct QueryPayload {
  std::uint32_t flags{0};
  std::uint32_t max_rows{0};
  std::uint32_t timeout_ms{0};
  std::string sql;
  bool has_script_metadata{false};
  std::uint64_t declared_script_size_bytes{0};
  std::uint32_t expected_statement_count{0};
  std::uint32_t script_block_size_hint{0};
};

bool LooksInteger(std::string_view value);
bool LooksDecimal(std::string_view value);

struct SimpleInsertRowsetPreparedEntry {
  std::string prepared_statement_uuid;
  std::string operation_id;
  std::string target_object_uuid;
  std::uint64_t catalog_epoch{0};
  std::uint64_t security_policy_epoch{0};
  std::uint64_t grant_epoch{0};
  std::uint64_t descriptor_epoch{0};
  std::uint64_t localized_name_epoch{0};
  std::uint64_t language_resource_epoch{0};
  std::uint64_t message_resource_epoch{0};
};

enum class SbwpTxnFinalityState : std::uint8_t {
  kUnknown = 0,
  kCommitted = 1,
  kRolledBack = 2,
  kRefused = 3,
  kPostInventorySecondaryFailure = 4,
  kNotFound = 5,
};

struct SbwpTxnCommitRequest {
  std::uint8_t legacy_flags{0};
  std::uint16_t contract_flags{0};
  std::array<std::uint8_t, 16> idempotency_key{};
  std::uint64_t request_fingerprint{0};
  std::uint64_t expected_txn_id{0};
};

struct SbwpTxnFinalityQuery {
  std::uint16_t flags{0};
  std::array<std::uint8_t, 16> idempotency_key{};
  std::array<std::uint8_t, 16> finality_token{};
  std::uint64_t expected_txn_id{0};
};

struct SbwpTxnFinalityRecord {
  SbwpTxnFinalityState state{SbwpTxnFinalityState::kUnknown};
  std::uint16_t flags{0};
  std::array<std::uint8_t, 16> idempotency_key{};
  std::array<std::uint8_t, 16> finality_token{};
  std::uint64_t request_fingerprint{0};
  std::uint64_t original_txn_id{0};
  std::uint64_t replacement_txn_id{0};
  std::string diagnostic_code;
  std::string detail;
};

struct SbwpSessionState {
  std::array<std::uint8_t, 16> attachment_id{};
  std::array<std::uint8_t, 16> session_uuid{};
  std::uint32_t server_sequence{0};
  std::uint64_t txn_id{0};
  std::uint64_t snapshot_visible_through_local_transaction_id{0};
  std::uint64_t catalog_epoch{0};
  std::uint64_t security_policy_epoch{0};
  std::uint64_t grant_epoch{0};
  std::uint64_t descriptor_epoch{0};
  bool authenticated{false};
  bool p1_payloads{false};
  bool ready_sent_for_current_operation{false};
  std::uint16_t selected_protocol_version{kSbwpVersionCurrent};
  std::uint64_t negotiated_features{0};
  std::string authenticated_user_uuid;
  std::string auth_provider_family;
  std::string principal_claim;
  std::string language_profile;
  std::string language_tag;
  std::string default_language_tag;
  std::string input_syntax_profile;
  std::string input_language_fallback_tag;
  std::string common_resource_hash;
  std::uint64_t language_resource_epoch{0};
  std::uint64_t localized_name_epoch{0};
  std::uint64_t message_resource_epoch{0};
  std::string resource_compatibility_identity;
  std::string resource_version_identity;
  std::map<std::string, std::string> session_parameters;
  std::map<std::string, PreparedStatement> statements;
  std::deque<std::string> statement_lru;
  std::size_t prepared_statement_bytes{0};
  std::map<std::string, BoundPortal> portals;
  std::deque<std::string> portal_lru;
  std::size_t bound_portal_bytes{0};
  CopyImportState copy_import;
  bool partial_query_active{false};
  std::vector<std::uint8_t> partial_query_payload;
  std::map<std::string, SimpleInsertRowsetPreparedEntry> simple_insert_rowset_cache;
  std::map<std::string, SbwpTxnFinalityRecord> finality_by_idempotency_key;
  std::map<std::string, std::string> idempotency_key_by_finality_token;
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

std::int32_t ReadI32(const std::vector<std::uint8_t>& data, std::size_t off) {
  return static_cast<std::int32_t>(ReadU32(data, off));
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

bool ParserPhaseTraceEnabled() {
  static const bool enabled = [] {
    const char* trace_path = std::getenv("SCRATCHBIRD_SBSQL_WORKER_PHASE_TRACE_FILE");
    return trace_path != nullptr && *trace_path != '\0';
  }();
  return enabled;
}

std::int64_t ParserPhaseNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string ParserPhaseJsonEscape(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          out += "\\u00";
          constexpr char hex[] = "0123456789abcdef";
          out.push_back(hex[(static_cast<unsigned char>(ch) >> 4) & 0x0f]);
          out.push_back(hex[static_cast<unsigned char>(ch) & 0x0f]);
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
  return out;
}

void WriteParserPhaseTrace(std::string_view event,
                           std::string_view phase,
                           std::int64_t elapsed_ns,
                           std::size_t bytes,
                           std::size_t count,
                           std::size_t rows,
                           std::string_view detail = {}) {
  const char* trace_path = std::getenv("SCRATCHBIRD_SBSQL_WORKER_PHASE_TRACE_FILE");
  if (trace_path == nullptr || *trace_path == '\0') return;
  static std::mutex trace_mutex;
  std::lock_guard<std::mutex> guard(trace_mutex);
  std::ofstream out(trace_path, std::ios::app);
  if (!out) return;
  out << "{\"event\":\"" << ParserPhaseJsonEscape(event)
      << "\",\"phase\":\"" << ParserPhaseJsonEscape(phase)
      << "\",\"elapsed_us\":"
      << (static_cast<double>(elapsed_ns) / 1000.0)
      << ",\"bytes\":" << static_cast<unsigned long long>(bytes)
      << ",\"count\":" << static_cast<unsigned long long>(count)
      << ",\"rows\":" << static_cast<unsigned long long>(rows)
      << ",\"detail\":\"" << ParserPhaseJsonEscape(detail) << "\"}\n";
}

void WriteParserPhaseTraceIfEnabled(bool enabled,
                                    std::string_view event,
                                    std::string_view phase,
                                    std::int64_t started_ns,
                                    std::size_t bytes,
                                    std::size_t count,
                                    std::size_t rows,
                                    std::string_view detail = {}) {
  if (!enabled) return;
  WriteParserPhaseTrace(event,
                        phase,
                        ParserPhaseNowNs() - started_ns,
                        bytes,
                        count,
                        rows,
                        detail);
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

void PutBytes(std::vector<std::uint8_t>* out,
              const std::uint8_t* data,
              std::size_t size) {
  out->insert(out->end(), data, data + size);
}

void PutLpStr(std::vector<std::uint8_t>* out, std::string_view value) {
  PutU32(out, static_cast<std::uint32_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

bool ReadLpStr(const std::vector<std::uint8_t>& payload,
               std::size_t* off,
               std::string* value) {
  if (*off + 4 > payload.size()) return false;
  const std::uint32_t length = ReadU32(payload, *off);
  *off += 4;
  if (*off + length > payload.size()) return false;
  value->assign(reinterpret_cast<const char*>(payload.data() + *off), length);
  *off += length;
  return true;
}

void PutU16Str(std::vector<std::uint8_t>* out, std::string_view value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

bool ReadU16Str(const std::vector<std::uint8_t>& payload,
                std::size_t* off,
                std::string* value) {
  if (*off + 2 > payload.size()) return false;
  const std::uint16_t length = ReadU16(payload, *off);
  *off += 2;
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

std::string Upper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return value;
}

std::string Trim(std::string value) {
  auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
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

std::size_t FindKeywordOutsideSql(std::string_view sql,
                                  std::string_view keyword,
                                  std::size_t start = 0) {
  bool in_single = false;
  bool in_double = false;
  bool in_line_comment = false;
  bool in_block_comment = false;
  std::size_t depth = 0;
  const std::string upper_keyword = Upper(std::string(keyword));
  for (std::size_t i = start; i < sql.size();) {
    const char ch = sql[i];
    const char next = i + 1 < sql.size() ? sql[i + 1] : '\0';
    if (in_line_comment) {
      in_line_comment = ch != '\n';
      ++i;
      continue;
    }
    if (in_block_comment) {
      if (ch == '*' && next == '/') {
        in_block_comment = false;
        i += 2;
      } else {
        ++i;
      }
      continue;
    }
    if (!in_single && !in_double && ch == '-' && next == '-') {
      in_line_comment = true;
      i += 2;
      continue;
    }
    if (!in_single && !in_double && ch == '/' && next == '*') {
      in_block_comment = true;
      i += 2;
      continue;
    }
    if (ch == '\'' && !in_double) {
      if (in_single && next == '\'') {
        i += 2;
      } else {
        in_single = !in_single;
        ++i;
      }
      continue;
    }
    if (ch == '"' && !in_single) {
      if (in_double && next == '"') {
        i += 2;
      } else {
        in_double = !in_double;
        ++i;
      }
      continue;
    }
    if (!in_single && !in_double) {
      if (ch == '(') {
        ++depth;
        ++i;
        continue;
      }
      if (ch == ')') {
        if (depth > 0) --depth;
        ++i;
        continue;
      }
      if (depth == 0 && i + upper_keyword.size() <= sql.size()) {
        const std::string candidate = Upper(std::string(sql.substr(i, upper_keyword.size())));
        const bool left_ok = i == 0 ||
            (!std::isalnum(static_cast<unsigned char>(sql[i - 1])) && sql[i - 1] != '_');
        const bool right_ok = i + upper_keyword.size() == sql.size() ||
            (!std::isalnum(static_cast<unsigned char>(sql[i + upper_keyword.size()])) &&
             sql[i + upper_keyword.size()] != '_');
        if (left_ok && right_ok && candidate == upper_keyword) return i;
      }
    }
    ++i;
  }
  return std::string_view::npos;
}

std::size_t FindMatchingSqlParen(std::string_view sql, std::size_t open_pos) {
  if (open_pos >= sql.size() || sql[open_pos] != '(') return std::string_view::npos;
  bool in_single = false;
  bool in_double = false;
  std::size_t depth = 0;
  for (std::size_t i = open_pos; i < sql.size(); ++i) {
    const char ch = sql[i];
    const char next = i + 1 < sql.size() ? sql[i + 1] : '\0';
    if (ch == '\'' && !in_double) {
      if (in_single && next == '\'') {
        ++i;
      } else {
        in_single = !in_single;
      }
      continue;
    }
    if (ch == '"' && !in_single) {
      if (in_double && next == '"') {
        ++i;
      } else {
        in_double = !in_double;
      }
      continue;
    }
    if (in_single || in_double) continue;
    if (ch == '(') {
      ++depth;
    } else if (ch == ')') {
      if (depth == 0) return std::string_view::npos;
      --depth;
      if (depth == 0) return i;
    }
  }
  return std::string_view::npos;
}

std::vector<std::string> SplitTopLevelComma(std::string_view text) {
  std::vector<std::string> out;
  bool in_single = false;
  bool in_double = false;
  std::size_t depth = 0;
  std::size_t start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    const char next = i + 1 < text.size() ? text[i + 1] : '\0';
    if (ch == '\'' && !in_double) {
      if (in_single && next == '\'') {
        ++i;
      } else {
        in_single = !in_single;
      }
      continue;
    }
    if (ch == '"' && !in_single) {
      if (in_double && next == '"') {
        ++i;
      } else {
        in_double = !in_double;
      }
      continue;
    }
    if (in_single || in_double) continue;
    if (ch == '(') {
      ++depth;
    } else if (ch == ')') {
      if (depth > 0) --depth;
    } else if (ch == ',' && depth == 0) {
      out.push_back(Trim(std::string(text.substr(start, i - start))));
      start = i + 1;
    }
  }
  out.push_back(Trim(std::string(text.substr(start))));
  return out;
}

std::string TrimIdentifierToken(std::string token) {
  token = Trim(std::move(token));
  if (token.size() >= 2 && token.front() == '"' && token.back() == '"') {
    std::string out;
    for (std::size_t i = 1; i + 1 < token.size(); ++i) {
      if (token[i] == '"' && i + 2 < token.size() && token[i + 1] == '"') {
        out.push_back('"');
        ++i;
      } else {
        out.push_back(token[i]);
      }
    }
    return out;
  }
  return token;
}

std::optional<std::size_t> PlaceholderIndexForValue(std::string_view token,
                                                    std::size_t* next_question_index) {
  const std::string trimmed = Trim(std::string(token));
  if (trimmed == "?") {
    if (next_question_index == nullptr) return std::nullopt;
    return (*next_question_index)++;
  }
  if (trimmed.size() > 1 && trimmed.front() == '$') {
    std::size_t value = 0;
    for (std::size_t i = 1; i < trimmed.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(trimmed[i]))) return std::nullopt;
      value = value * 10u + static_cast<std::size_t>(trimmed[i] - '0');
    }
    if (value == 0) return std::nullopt;
    return value - 1;
  }
  return std::nullopt;
}

std::string StripLeadingSqlComments(std::string sql) {
  std::size_t pos = 0;
  while (pos < sql.size()) {
    while (pos < sql.size() &&
           std::isspace(static_cast<unsigned char>(sql[pos]))) {
      ++pos;
    }
    if (pos + 1 < sql.size() && sql[pos] == '-' && sql[pos + 1] == '-') {
      const std::size_t newline = sql.find('\n', pos + 2);
      if (newline == std::string::npos) return {};
      pos = newline + 1;
      continue;
    }
    if (pos + 1 < sql.size() && sql[pos] == '/' && sql[pos + 1] == '*') {
      const std::size_t close = sql.find("*/", pos + 2);
      if (close == std::string::npos) return {};
      pos = close + 2;
      continue;
    }
    break;
  }
  return pos == 0 ? std::move(sql) : sql.substr(pos);
}

std::optional<PreparedStatement::InsertRowsetPlan> AnalyzePreparedInsertRowset(std::string_view raw_sql) {
  const std::string sql = StripSqlTerminator(std::string(raw_sql));
  const std::string upper = Upper(sql);
  if (!StartsWithWord(upper, "INSERT")) return std::nullopt;
  const std::size_t into_pos = FindKeywordOutsideSql(sql, "INTO");
  if (into_pos == std::string_view::npos) return std::nullopt;
  std::size_t pos = into_pos + 4;
  while (pos < sql.size() && std::isspace(static_cast<unsigned char>(sql[pos]))) ++pos;
  if (pos >= sql.size()) return std::nullopt;
  const std::size_t target_start = pos;
  bool in_double = false;
  while (pos < sql.size()) {
    const char ch = sql[pos];
    const char next = pos + 1 < sql.size() ? sql[pos + 1] : '\0';
    if (ch == '"') {
      if (in_double && next == '"') {
        pos += 2;
        continue;
      }
      in_double = !in_double;
      ++pos;
      continue;
    }
    if (!in_double && (std::isspace(static_cast<unsigned char>(ch)) || ch == '(')) break;
    ++pos;
  }
  const std::string target_name = Trim(std::string(sql.substr(target_start, pos - target_start)));
  if (target_name.empty()) return std::nullopt;
  while (pos < sql.size() && std::isspace(static_cast<unsigned char>(sql[pos]))) ++pos;
  if (pos >= sql.size() || sql[pos] != '(') return std::nullopt;
  const std::size_t columns_end = FindMatchingSqlParen(sql, pos);
  if (columns_end == std::string_view::npos) return std::nullopt;
  auto columns = SplitTopLevelComma(std::string_view(sql).substr(pos + 1, columns_end - pos - 1));
  if (columns.empty()) return std::nullopt;
  for (auto& column : columns) {
    column = TrimIdentifierToken(std::move(column));
    if (column.empty()) return std::nullopt;
  }
  const std::size_t values_pos = FindKeywordOutsideSql(sql, "VALUES", columns_end + 1);
  if (values_pos == std::string_view::npos) return std::nullopt;
  pos = values_pos + 6;
  while (pos < sql.size() && std::isspace(static_cast<unsigned char>(sql[pos]))) ++pos;
  if (pos >= sql.size() || sql[pos] != '(') return std::nullopt;
  const std::size_t values_end = FindMatchingSqlParen(sql, pos);
  if (values_end == std::string_view::npos) return std::nullopt;
  auto values = SplitTopLevelComma(std::string_view(sql).substr(pos + 1, values_end - pos - 1));
  if (values.size() != columns.size()) return std::nullopt;
  std::vector<std::size_t> parameter_indexes;
  parameter_indexes.reserve(values.size());
  std::size_t next_question_index = 0;
  for (const auto& value : values) {
    auto index = PlaceholderIndexForValue(value, &next_question_index);
    if (!index.has_value()) return std::nullopt;
    parameter_indexes.push_back(*index);
  }
  PreparedStatement::InsertRowsetPlan plan;
  plan.target_name = target_name;
  plan.column_names = std::move(columns);
  plan.parameter_indexes = std::move(parameter_indexes);
  return plan;
}

bool DecodeSimpleSqlStringLiteral(std::string_view token, std::string* out) {
  if (token.size() < 2 || token.front() != '\'' || token.back() != '\'') return false;
  std::string decoded;
  decoded.reserve(token.size() - 2);
  for (std::size_t i = 1; i + 1 < token.size(); ++i) {
    const char ch = token[i];
    if (ch == '\'') {
      if (i + 2 < token.size() && token[i + 1] == '\'') {
        decoded.push_back('\'');
        ++i;
        continue;
      }
      return false;
    }
    decoded.push_back(ch);
  }
  *out = std::move(decoded);
  return true;
}

bool ParseSimpleInsertLiteralValue(std::string_view raw,
                                   std::optional<std::string>* value) {
  std::string token = Trim(std::string(raw));
  if (token.empty()) return false;
  const std::string upper = Upper(token);
  if (upper == "NULL") {
    *value = std::nullopt;
    return true;
  }
  if (upper == "DEFAULT") return false;
  std::string decoded;
  if (DecodeSimpleSqlStringLiteral(token, &decoded)) {
    *value = std::move(decoded);
    return true;
  }
  if (upper == "TRUE" || upper == "FALSE" || upper == "T" || upper == "F" ||
      LooksInteger(token) || LooksDecimal(token)) {
    *value = std::move(token);
    return true;
  }
  return false;
}

std::optional<CopyImportState> AnalyzeSimpleLiteralInsertRowset(std::string_view raw_sql,
                                                                bool allow_single_row = false) {
  const std::string sql = StripLeadingSqlComments(
      StripSqlTerminator(std::string(raw_sql)));
  const std::string upper = Upper(sql);
  if (!StartsWithWord(upper, "INSERT")) return std::nullopt;
  const std::size_t into_pos = FindKeywordOutsideSql(sql, "INTO");
  if (into_pos == std::string_view::npos) return std::nullopt;
  std::size_t pos = into_pos + 4;
  while (pos < sql.size() && std::isspace(static_cast<unsigned char>(sql[pos]))) ++pos;
  if (pos >= sql.size()) return std::nullopt;
  const std::size_t target_start = pos;
  bool in_double = false;
  while (pos < sql.size()) {
    const char ch = sql[pos];
    const char next = pos + 1 < sql.size() ? sql[pos + 1] : '\0';
    if (ch == '"') {
      if (in_double && next == '"') {
        pos += 2;
        continue;
      }
      in_double = !in_double;
      ++pos;
      continue;
    }
    if (!in_double && (std::isspace(static_cast<unsigned char>(ch)) || ch == '(')) break;
    ++pos;
  }
  const std::string target_name = Trim(std::string(sql.substr(target_start, pos - target_start)));
  if (target_name.empty()) return std::nullopt;
  while (pos < sql.size() && std::isspace(static_cast<unsigned char>(sql[pos]))) ++pos;
  if (pos >= sql.size() || sql[pos] != '(') return std::nullopt;
  const std::size_t columns_end = FindMatchingSqlParen(sql, pos);
  if (columns_end == std::string_view::npos) return std::nullopt;
  auto columns = SplitTopLevelComma(std::string_view(sql).substr(pos + 1, columns_end - pos - 1));
  if (columns.empty()) return std::nullopt;
  for (auto& column : columns) {
    column = TrimIdentifierToken(std::move(column));
    if (column.empty()) return std::nullopt;
  }
  const std::size_t values_pos = FindKeywordOutsideSql(sql, "VALUES", columns_end + 1);
  if (values_pos == std::string_view::npos) return std::nullopt;
  const std::string between = Trim(std::string(
      std::string_view(sql).substr(columns_end + 1, values_pos - columns_end - 1)));
  if (!between.empty()) return std::nullopt;

  CopyImportState rowset;
  rowset.native_bulk_ingest = true;
  rowset.native_bulk_ingest_enabled = true;
  rowset.sql = sql;
  rowset.target_name = target_name;
  rowset.source_size_bytes = sql.size();
  rowset.preallocation_bytes = sql.size();
  rowset.preallocation_factor_percent = 100;
  rowset.format_family = "binary_typed_rows";

  pos = values_pos + 6;
  while (pos < sql.size() && std::isspace(static_cast<unsigned char>(sql[pos]))) ++pos;
  while (pos < sql.size()) {
    if (sql[pos] != '(') return std::nullopt;
    const std::size_t row_end = FindMatchingSqlParen(sql, pos);
    if (row_end == std::string_view::npos) return std::nullopt;
    auto values = SplitTopLevelComma(std::string_view(sql).substr(pos + 1, row_end - pos - 1));
    if (values.size() != columns.size()) return std::nullopt;
    CopyImportRow row;
    row.fields.reserve(columns.size());
    for (std::size_t column_index = 0; column_index < columns.size(); ++column_index) {
      std::optional<std::string> value;
      if (!ParseSimpleInsertLiteralValue(values[column_index], &value)) return std::nullopt;
      row.fields.emplace_back(columns[column_index], std::move(value));
    }
    rowset.rows.push_back(std::move(row));
    pos = row_end + 1;
    while (pos < sql.size() && std::isspace(static_cast<unsigned char>(sql[pos]))) ++pos;
    if (pos == sql.size()) break;
    if (sql[pos] != ',') return std::nullopt;
    ++pos;
    while (pos < sql.size() && std::isspace(static_cast<unsigned char>(sql[pos]))) ++pos;
  }
  if (!allow_single_row && rowset.rows.size() < 2) {
    return std::nullopt;
  }
  return rowset;
}

std::string SimpleInsertRowsetCacheKey(const CopyImportState& rowset) {
  std::string key = rowset.target_object_uuid;
  if (!rowset.rows.empty()) {
    for (const auto& [name, _] : rowset.rows.front().fields) {
      key.push_back('\x1f');
      key += name;
    }
  }
  return key;
}

std::string SimpleInsertRowsetPresentedShapeKey(const CopyImportState& rowset) {
  std::string key = rowset.target_name;
  if (!rowset.rows.empty()) {
    for (const auto& [name, _] : rowset.rows.front().fields) {
      key.push_back('\x1f');
      key += name;
    }
  }
  return key;
}

void RefreshWireAuthorityEpochsFromSession(const SbsqlTestWireSession& session,
                                           SbwpSessionState* state) {
  if (state == nullptr) return;
  const auto& context = session.session();
  state->catalog_epoch = context.catalog_epoch;
  state->security_policy_epoch = context.security_policy_epoch;
  state->grant_epoch = context.grant_epoch;
  state->descriptor_epoch = context.descriptor_epoch;
  state->language_resource_epoch = context.language_resource_epoch;
  state->localized_name_epoch = context.localized_name_epoch;
  state->message_resource_epoch = context.message_resource_epoch;
}

bool SimpleInsertRowsetPreparedEntryCurrent(
    const SimpleInsertRowsetPreparedEntry& entry,
    const SessionContext& context) {
  return !entry.prepared_statement_uuid.empty() &&
         !entry.target_object_uuid.empty() &&
         entry.catalog_epoch == context.catalog_epoch &&
         entry.security_policy_epoch == context.security_policy_epoch &&
         entry.grant_epoch == context.grant_epoch &&
         entry.descriptor_epoch == context.descriptor_epoch &&
         entry.localized_name_epoch == context.localized_name_epoch &&
         entry.language_resource_epoch == context.language_resource_epoch &&
         entry.message_resource_epoch == context.message_resource_epoch;
}

void CaptureSimpleInsertRowsetAuthorityEpochs(
    const SbsqlTestWireSession& session,
    SimpleInsertRowsetPreparedEntry* entry) {
  if (entry == nullptr) return;
  const auto& context = session.session();
  entry->catalog_epoch = context.catalog_epoch;
  entry->security_policy_epoch = context.security_policy_epoch;
  entry->grant_epoch = context.grant_epoch;
  entry->descriptor_epoch = context.descriptor_epoch;
  entry->localized_name_epoch = context.localized_name_epoch;
  entry->language_resource_epoch = context.language_resource_epoch;
  entry->message_resource_epoch = context.message_resource_epoch;
}

bool CopyPreparedHandleCurrent(const CopyImportState& copy,
                               const SessionContext& context) {
  return !copy.prepared_statement_uuid.empty() &&
         !copy.target_object_uuid.empty() &&
         copy.prepared_catalog_epoch == context.catalog_epoch &&
         copy.prepared_security_policy_epoch == context.security_policy_epoch &&
         copy.prepared_grant_epoch == context.grant_epoch &&
         copy.prepared_descriptor_epoch == context.descriptor_epoch &&
         copy.prepared_localized_name_epoch == context.localized_name_epoch &&
         copy.prepared_language_resource_epoch == context.language_resource_epoch &&
         copy.prepared_message_resource_epoch == context.message_resource_epoch;
}

void CaptureCopyPreparedAuthorityEpochs(const SbsqlTestWireSession& session,
                                        CopyImportState* copy) {
  if (copy == nullptr) return;
  const auto& context = session.session();
  copy->prepared_catalog_epoch = context.catalog_epoch;
  copy->prepared_security_policy_epoch = context.security_policy_epoch;
  copy->prepared_grant_epoch = context.grant_epoch;
  copy->prepared_descriptor_epoch = context.descriptor_epoch;
  copy->prepared_localized_name_epoch = context.localized_name_epoch;
  copy->prepared_language_resource_epoch = context.language_resource_epoch;
  copy->prepared_message_resource_epoch = context.message_resource_epoch;
}

bool SbwpOperationInvalidatesSimpleInsertPreparedCache(std::string_view operation_id) {
  return operation_id.rfind("ddl.", 0) == 0 ||
         operation_id.rfind("catalog.", 0) == 0 ||
         operation_id.rfind("security.", 0) == 0 ||
         operation_id.rfind("language.", 0) == 0 ||
         operation_id.rfind("policy.", 0) == 0 ||
         operation_id.rfind("auth.", 0) == 0;
}

std::string ScriptChunkSetTermDirective(const std::string& chunk) {
  std::istringstream lines(chunk);
  std::string meaningful;
  std::string line;
  while (std::getline(lines, line)) {
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed.starts_with("--")) continue;
    if (!meaningful.empty()) meaningful.push_back(' ');
    meaningful += trimmed;
  }
  const std::string upper = Upper(meaningful);
  if (!StartsWithWord(upper, "SET") ||
      upper.find("TERM") == std::string::npos ||
      upper.find("SET TERM") != 0) {
    return {};
  }
  return Trim(meaningful.substr(8));
}

std::optional<std::string> TryReadDollarQuoteTag(std::string_view script,
                                                 std::size_t offset) {
  if (offset >= script.size() || script[offset] != '$') return std::nullopt;
  std::size_t cursor = offset + 1;
  while (cursor < script.size()) {
    const char ch = script[cursor];
    if (ch == '$') {
      return std::string(script.substr(offset, cursor - offset + 1));
    }
    const auto uch = static_cast<unsigned char>(ch);
    if (!std::isalnum(uch) && ch != '_') return std::nullopt;
    ++cursor;
  }
  return std::nullopt;
}

std::vector<std::string> SplitSbwpScriptStatements(std::string_view script) {
  std::vector<std::string> statements;
  std::string current;
  std::string term = ";";
  bool in_single = false;
  bool in_double = false;
  bool in_line_comment = false;
  bool in_block_comment = false;
  std::string dollar_quote_tag;

  const auto flush = [&]() {
    const std::string chunk = Trim(current);
    current.clear();
    if (chunk.empty()) return;
    const std::string new_term = ScriptChunkSetTermDirective(chunk);
    if (!new_term.empty()) {
      term = new_term;
      return;
    }
    statements.push_back(chunk);
  };

  for (std::size_t i = 0; i < script.size();) {
    const char ch = script[i];
    const char next = i + 1 < script.size() ? script[i + 1] : '\0';
    if (in_line_comment) {
      current.push_back(ch);
      ++i;
      if (ch == '\n') in_line_comment = false;
      continue;
    }
    if (in_block_comment) {
      current.push_back(ch);
      if (ch == '*' && next == '/') {
        current.push_back(next);
        i += 2;
        in_block_comment = false;
      } else {
        ++i;
      }
      continue;
    }
    if (!dollar_quote_tag.empty()) {
      if (script.compare(i, dollar_quote_tag.size(), dollar_quote_tag) == 0) {
        current.append(dollar_quote_tag);
        i += dollar_quote_tag.size();
        dollar_quote_tag.clear();
      } else {
        current.push_back(ch);
        ++i;
      }
      continue;
    }
    if (!in_single && !in_double && ch == '-' && next == '-') {
      current.push_back(ch);
      current.push_back(next);
      i += 2;
      in_line_comment = true;
      continue;
    }
    if (!in_single && !in_double && ch == '/' && next == '*') {
      current.push_back(ch);
      current.push_back(next);
      i += 2;
      in_block_comment = true;
      continue;
    }
    if (!in_single && !in_double && ch == '$') {
      if (auto tag = TryReadDollarQuoteTag(script, i)) {
        dollar_quote_tag = *tag;
        current.append(*tag);
        i += tag->size();
        continue;
      }
    }
    if (ch == '\'' && !in_double) {
      current.push_back(ch);
      if (in_single && next == '\'') {
        current.push_back(next);
        i += 2;
        continue;
      }
      in_single = !in_single;
      ++i;
      continue;
    }
    if (ch == '"' && !in_single) {
      current.push_back(ch);
      if (in_double && next == '"') {
        current.push_back(next);
        i += 2;
        continue;
      }
      in_double = !in_double;
      ++i;
      continue;
    }
    if (!in_single && !in_double && !term.empty() &&
        script.compare(i, term.size(), term) == 0) {
      const std::size_t matched = term.size();
      flush();
      i += matched;
      continue;
    }
    current.push_back(ch);
    ++i;
  }
  flush();
  return statements;
}

bool CompatibleSimpleInsertRowsetsForScript(const CopyImportState& existing,
                                            const CopyImportState& candidate) {
  if (existing.target_name != candidate.target_name) return false;
  if (existing.rows.empty() || candidate.rows.empty()) return false;
  const auto& left = existing.rows.front().fields;
  const auto& right = candidate.rows.front().fields;
  if (left.size() != right.size()) return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (left[i].first != right[i].first) return false;
  }
  return true;
}

void AppendSimpleInsertRowsForScript(CopyImportState* existing,
                                     const CopyImportState& candidate) {
  if (existing == nullptr) return;
  existing->rows.insert(existing->rows.end(),
                        candidate.rows.begin(),
                        candidate.rows.end());
  existing->source_size_bytes += candidate.source_size_bytes;
  existing->preallocation_bytes += candidate.preallocation_bytes;
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

std::size_t PreparedStatementBytes(const PreparedStatement& statement) {
  std::size_t bytes = statement.sql.size() + statement.param_types.size() * sizeof(std::uint32_t);
  if (statement.insert_rowset_plan.has_value()) {
    bytes += statement.insert_rowset_plan->target_name.size();
    bytes += statement.insert_rowset_plan->target_object_uuid.size();
    bytes += statement.insert_rowset_plan->server_prepared_statement_uuid.size();
    bytes += statement.insert_rowset_plan->server_operation_id.size();
    for (const auto& column : statement.insert_rowset_plan->column_names) bytes += column.size();
    bytes += statement.insert_rowset_plan->parameter_indexes.size() * sizeof(std::size_t);
  }
  return bytes;
}

std::size_t BoundPortalBytes(const BoundPortal& portal) {
  std::size_t bytes = portal.sql.size() + portal.param_types.size() * sizeof(std::uint32_t);
  for (const auto& value : portal.param_values) {
    if (value.has_value()) bytes += value->size();
  }
  for (const auto& row : portal.param_rows) {
    bytes += row.size() * sizeof(std::optional<std::string>);
    for (const auto& value : row) {
      if (value.has_value()) bytes += value->size();
    }
  }
  if (portal.insert_rowset_plan.has_value()) {
    bytes += portal.insert_rowset_plan->target_name.size();
    bytes += portal.insert_rowset_plan->target_object_uuid.size();
    bytes += portal.insert_rowset_plan->server_prepared_statement_uuid.size();
    bytes += portal.insert_rowset_plan->server_operation_id.size();
    for (const auto& column : portal.insert_rowset_plan->column_names) bytes += column.size();
    bytes += portal.insert_rowset_plan->parameter_indexes.size() * sizeof(std::size_t);
  }
  return bytes;
}

void RemovePreparedStatement(SbwpSessionState* state, const std::string& name) {
  if (state == nullptr) return;
  if (const auto found = state->statements.find(name); found != state->statements.end()) {
    state->prepared_statement_bytes -=
        std::min(state->prepared_statement_bytes, PreparedStatementBytes(found->second));
    state->statements.erase(found);
  }
  state->statement_lru.erase(std::remove(state->statement_lru.begin(),
                                         state->statement_lru.end(),
                                         name),
                             state->statement_lru.end());
}

bool StorePreparedStatement(SbwpSessionState* state,
                            const std::string& name,
                            PreparedStatement statement) {
  if (state == nullptr) return false;
  const std::size_t bytes = PreparedStatementBytes(statement);
  if (bytes > kMaxPreparedStatementSqlBytesPerSession) return false;
  RemovePreparedStatement(state, name);
  state->prepared_statement_bytes += bytes;
  state->statement_lru.push_back(name);
  state->statements[name] = std::move(statement);
  while ((state->statements.size() > kMaxPreparedStatementsPerSession ||
          state->prepared_statement_bytes > kMaxPreparedStatementSqlBytesPerSession) &&
         !state->statement_lru.empty()) {
    RemovePreparedStatement(state, state->statement_lru.front());
  }
  return true;
}

void RemoveBoundPortal(SbwpSessionState* state, const std::string& name) {
  if (state == nullptr) return;
  if (const auto found = state->portals.find(name); found != state->portals.end()) {
    state->bound_portal_bytes -= std::min(state->bound_portal_bytes, BoundPortalBytes(found->second));
    state->portals.erase(found);
  }
  state->portal_lru.erase(std::remove(state->portal_lru.begin(), state->portal_lru.end(), name),
                          state->portal_lru.end());
}

bool StoreBoundPortal(SbwpSessionState* state, const std::string& name, BoundPortal portal) {
  if (state == nullptr) return false;
  const std::size_t bytes = BoundPortalBytes(portal);
  if (bytes > kMaxBoundPortalSqlBytesPerSession) return false;
  RemoveBoundPortal(state, name);
  state->bound_portal_bytes += bytes;
  state->portal_lru.push_back(name);
  state->portals[name] = std::move(portal);
  while ((state->portals.size() > kMaxBoundPortalsPerSession ||
          state->bound_portal_bytes > kMaxBoundPortalSqlBytesPerSession) &&
         !state->portal_lru.empty()) {
    RemoveBoundPortal(state, state->portal_lru.front());
  }
  return true;
}

bool IsSafeSavepointIdentifier(std::string_view name) {
  if (name.empty()) return false;
  const unsigned char first = static_cast<unsigned char>(name.front());
  if (!std::isalpha(first) && name.front() != '_') return false;
  return std::all_of(name.begin() + 1, name.end(), [](char ch) {
    const unsigned char byte = static_cast<unsigned char>(ch);
    return std::isalnum(byte) || ch == '_';
  });
}

std::optional<std::string> SavepointSqlFromFrame(const Frame& frame, std::string_view prefix) {
  std::size_t off = 0;
  const std::string name = ReadSizedString(frame.payload, &off);
  if (off != frame.payload.size() || !IsSafeSavepointIdentifier(name)) {
    return std::nullopt;
  }
  std::string sql(prefix);
  sql += name;
  return sql;
}

std::string ReadNullTerminatedSql(const std::vector<std::uint8_t>& payload, std::size_t off) {
  if (off >= payload.size()) return {};
  std::size_t end = off;
  while (end < payload.size() && payload[end] != 0) ++end;
  return std::string(reinterpret_cast<const char*>(payload.data() + off), end - off);
}

QueryPayload ParseQueryPayload(const std::vector<std::uint8_t>& payload) {
  QueryPayload query;
  if (payload.size() < 12) return query;
  query.flags = ReadU32(payload, 0);
  query.max_rows = ReadU32(payload, 4);
  query.timeout_ms = ReadU32(payload, 8);
  std::size_t end = 12;
  while (end < payload.size() && payload[end] != 0) ++end;
  if (end >= payload.size()) {
    query.sql = ReadNullTerminatedSql(payload, 12);
    return query;
  }
  query.sql.assign(reinterpret_cast<const char*>(payload.data() + 12), end - 12);
  std::size_t off = end + 1;
  if (off + 24 <= payload.size() &&
      ReadU32(payload, off) == kQueryScriptMetadataMagic &&
      ReadU16(payload, off + 4) == kQueryScriptMetadataVersion) {
    query.has_script_metadata = true;
    query.flags |= ReadU16(payload, off + 6);
    query.declared_script_size_bytes = ReadU64(payload, off + 8);
    query.expected_statement_count = ReadU32(payload, off + 16);
    query.script_block_size_hint = ReadU32(payload, off + 20);
  }
  return query;
}

std::string ParseQuerySql(const std::vector<std::uint8_t>& payload) {
  return ParseQueryPayload(payload).sql;
}

std::uint32_t ParseQueryFlags(const std::vector<std::uint8_t>& payload) {
  return ParseQueryPayload(payload).flags;
}

bool ParseSetOptionPayload(const std::vector<std::uint8_t>& payload,
                           std::string* name,
                           std::string* value) {
  if (name == nullptr || value == nullptr) return false;
  std::size_t off = 0;
  if (!ReadLpStr(payload, &off, name)) return false;
  if (!ReadLpStr(payload, &off, value)) return false;
  if (off != payload.size()) return false;
  return !name->empty() && name->size() <= 256 && value->size() <= 4096;
}

std::size_t InferPlaceholderCount(std::string_view sql) {
  std::size_t question_count = 0;
  std::size_t max_dollar_index = 0;
  bool in_single = false;
  bool in_double = false;
  bool in_line_comment = false;
  bool in_block_comment = false;
  for (std::size_t index = 0; index < sql.size();) {
    const char ch = sql[index];
    const char next = index + 1 < sql.size() ? sql[index + 1] : '\0';
    if (in_line_comment) {
      in_line_comment = ch != '\n';
      ++index;
      continue;
    }
    if (in_block_comment) {
      if (ch == '*' && next == '/') {
        in_block_comment = false;
        index += 2;
      } else {
        ++index;
      }
      continue;
    }
    if (!in_single && !in_double && ch == '-' && next == '-') {
      in_line_comment = true;
      index += 2;
      continue;
    }
    if (!in_single && !in_double && ch == '/' && next == '*') {
      in_block_comment = true;
      index += 2;
      continue;
    }
    if (ch == '\'' && !in_double) {
      if (in_single && next == '\'') {
        index += 2;
        continue;
      }
      in_single = !in_single;
      ++index;
      continue;
    }
    if (ch == '"' && !in_single) {
      if (in_double && next == '"') {
        index += 2;
        continue;
      }
      in_double = !in_double;
      ++index;
      continue;
    }
    if (!in_single && !in_double && ch == '?') {
      ++question_count;
      ++index;
      continue;
    }
    if (!in_single && !in_double && ch == '$' && index + 1 < sql.size() &&
        std::isdigit(static_cast<unsigned char>(sql[index + 1]))) {
      std::size_t cursor = index + 1;
      std::size_t value = 0;
      while (cursor < sql.size() && std::isdigit(static_cast<unsigned char>(sql[cursor]))) {
        value = value * 10u + static_cast<std::size_t>(sql[cursor] - '0');
        ++cursor;
      }
      max_dollar_index = std::max(max_dollar_index, value);
      index = cursor;
      continue;
    }
    ++index;
  }
  return std::max(question_count, max_dollar_index);
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
  if (prepared.param_types.empty()) {
    prepared.param_types.resize(InferPlaceholderCount(prepared.sql), 0);
  }
  prepared.insert_rowset_plan = AnalyzePreparedInsertRowset(prepared.sql);
  return prepared;
}

std::map<std::string, std::string> ParseStartupParams(const std::vector<std::uint8_t>& payload) {
  std::map<std::string, std::string> out;
  if (payload.size() < 12) return out;
  std::size_t off = 12;
  auto read_cstring = [&](std::string* value) -> bool {
    if (off >= payload.size()) return false;
    const auto start = off;
    while (off < payload.size() && payload[off] != 0) ++off;
    if (off >= payload.size()) return false;
    value->assign(reinterpret_cast<const char*>(payload.data() + start), off - start);
    ++off;
    return true;
  };
  for (;;) {
    std::string key;
    if (!read_cstring(&key)) break;
    if (key.empty()) break;
    std::string value;
    if (!read_cstring(&value)) break;
    out[std::move(key)] = std::move(value);
  }
  return out;
}

bool IsKnownConnectKey(std::string_view key) {
  return key == "application_name" ||
         key == "auth_forbidden_methods" ||
         key == "auth_method_id" ||
         key == "auth_method_payload" ||
         key == "auth_payload_b64" ||
         key == "auth_payload_json" ||
         key == "auth_provider_profile" ||
         key == "auth_required_methods" ||
         key == "auth_require_channel_binding" ||
         key == "calendar" ||
         key == "charset" ||
         key == "client_flags" ||
         key == "client_encoding" ||
         key == "database" ||
         key == "decimal_locale" ||
         key == "default_catalog" ||
         key == "default_schema" ||
         key == "keepalive_interval_ms" ||
         key == "keepalive_timeout_ms" ||
         key == "language" ||
         key == "proxy_principal_assertion" ||
         key == "role" ||
         key == "statement_timeout_ms" ||
         key == "timezone" ||
         key == "time_zone" ||
         key == "traceparent" ||
         key == "tracestate" ||
         key == "user" ||
         key == "workload_identity_token";
}

bool LooksLikeP1Startup(const std::vector<std::uint8_t>& payload) {
  if (payload.size() < 84) return false;
  // Legacy startup begins with nul-terminated ASCII keys. P1 startup begins with
  // little-endian version words; keep unsupported P1 windows on the P1 path so
  // they fail closed instead of being reinterpreted as legacy key/value bytes.
  return payload[1] <= 0x02u && payload[3] <= 0x02u;
}

struct StartupNegotiation {
  bool p1{false};
  bool ok{true};
  bool close_after_error{false};
  std::string sqlstate{"08P01"};
  std::string message;
  std::string detail;
  std::map<std::string, std::string> params;
  std::uint16_t selected_protocol_version{kSbwpVersionCurrent};
  std::uint64_t negotiated_features{0};
};

std::string DecodeConnectValue(std::uint8_t value_type,
                               const std::vector<std::uint8_t>& payload,
                               std::size_t value_offset,
                               std::uint32_t value_length) {
  if (value_offset + value_length > payload.size()) return {};
  const auto* data = payload.data() + value_offset;
  switch (value_type) {
    case 0x01:
    case 0x05:
    case 0x06:
      return std::string(reinterpret_cast<const char*>(data), value_length);
    case 0x02:
      if (value_length == 8) return std::to_string(ReadU64(payload, value_offset));
      if (value_length == 4) return std::to_string(ReadU32(payload, value_offset));
      return {};
    case 0x03:
      return value_length > 0 && data[0] != 0 ? "true" : "false";
    case 0x04: {
      if (value_length != 16) return {};
      static constexpr char kHex[] = "0123456789abcdef";
      std::string out;
      out.reserve(36);
      for (std::size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(kHex[data[i] >> 4u]);
        out.push_back(kHex[data[i] & 0x0fu]);
      }
      return out;
    }
    default:
      return {};
  }
}

StartupNegotiation RejectStartup(std::string sqlstate,
                                 std::string message,
                                 std::string detail) {
  StartupNegotiation rejected;
  rejected.p1 = true;
  rejected.ok = false;
  rejected.close_after_error = true;
  rejected.sqlstate = std::move(sqlstate);
  rejected.message = std::move(message);
  rejected.detail = std::move(detail);
  return rejected;
}

StartupNegotiation ParseStartupNegotiation(const std::vector<std::uint8_t>& payload) {
  StartupNegotiation negotiated;
  if (!LooksLikeP1Startup(payload)) {
    negotiated.params = ParseStartupParams(payload);
    negotiated.negotiated_features = 0;
    return negotiated;
  }

  negotiated.p1 = true;
  const auto min_version = ReadU16(payload, 0);
  const auto max_version = ReadU16(payload, 2);
  const auto connect_flags = ReadU32(payload, 4);
  const auto client_features = ReadU64(payload, 8);
  const auto required_features = ReadU64(payload, 16);
  const auto optional_features = ReadU64(payload, 24);
  if (min_version > max_version) {
    return RejectStartup("08P01",
                         "SBWP.VERSION.NO_COMMON_VERSION",
                         "startup protocol_min_version is greater than protocol_max_version");
  }
  if (max_version < kSbwpVersionMin || min_version > kSbwpVersionCurrent) {
    return RejectStartup("08P01",
                         "SBWP.VERSION.NO_COMMON_VERSION",
                         "client and server protocol version windows do not overlap");
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
                         "required P1 native-wire feature is unavailable on the selected downgraded version");
  }
  const auto unknown_required = required_features & ~kKnownCoreFeatureMask;
  if (unknown_required != 0) {
    return RejectStartup("08P01",
                         "SBWP.FEATURE.UNKNOWN_REQUIRED",
                         "startup required an unknown core feature bit");
  }
  if ((required_features & ~client_features) != 0) {
    return RejectStartup("08P01",
                         "NATIVE_WIRE.CONNECT_INVALID_PAYLOAD",
                         "startup required a feature absent from client_feature_bitmap");
  }
  const auto unsupported_required = required_features & ~kServerSupportedFeatureMask;
  if (unsupported_required != 0) {
    return RejectStartup("0A000",
                         "SBWP.FEATURE.REQUIRED_UNSUPPORTED",
                         "startup required a core feature this parser route does not support");
  }
  std::uint64_t supported = kServerSupportedFeatureMask;
  if (negotiated.selected_protocol_version < kSbwpVersionCurrent) {
    supported &= ~kP1OnlyFeatureMask;
  }
  negotiated.negotiated_features = client_features & supported;
  (void)optional_features;

  std::size_t off = 80;
  const auto connect_key_count = ReadU32(payload, off);
  off += 4;
  if (connect_key_count > 256) {
    return RejectStartup("08P01",
                         "NATIVE_WIRE.CONNECT_INVALID_PAYLOAD",
                         "startup connect key count exceeds the protocol limit");
  }
  for (std::uint32_t i = 0; i < connect_key_count; ++i) {
    std::string key;
    if (!ReadLpStr(payload, &off, &key) || off + 6 > payload.size()) {
      return RejectStartup("08P01",
                           "NATIVE_WIRE.CONNECT_INVALID_PAYLOAD",
                           "startup connect key payload is truncated");
    }
    const std::uint8_t value_type = payload[off++];
    const std::uint8_t redaction_class = payload[off++];
    const std::uint32_t value_length = ReadU32(payload, off);
    off += 4;
    if (off + value_length > payload.size() || !IsKnownConnectKey(key)) {
      return RejectStartup("08P01",
                           IsKnownConnectKey(key) ? "NATIVE_WIRE.CONNECT_INVALID_PAYLOAD"
                                                  : "NATIVE_WIRE.CONNECT_UNKNOWN_KEY",
                           "startup connect key is malformed or not in the admitted registry");
    }
    const auto value = DecodeConnectValue(value_type, payload, off, value_length);
    off += value_length;
    if (redaction_class < 3) {
      negotiated.params[std::move(key)] = value;
    }
  }
  if (off + 4 > payload.size()) {
    return RejectStartup("08P01",
                         "NATIVE_WIRE.CONNECT_INVALID_PAYLOAD",
                         "startup extension offer count is missing");
  }
  const auto extension_count = ReadU32(payload, off);
  off += 4;
  if (extension_count > 64) {
    return RejectStartup("08P01",
                         "SBWP.EXTENSION.REQUIRED_UNSUPPORTED",
                         "startup extension offer count exceeds the protocol limit");
  }
  for (std::uint32_t i = 0; i < extension_count; ++i) {
    std::string extension_name;
    if (!ReadLpStr(payload, &off, &extension_name) || off + 10 > payload.size()) {
      return RejectStartup("08P01",
                           "SBWP.EXTENSION.REQUIRED_UNSUPPORTED",
                           "startup extension offer is truncated");
    }
    off += 2;  // major
    off += 2;  // min minor
    off += 2;  // max minor
    const auto flags = ReadU32(payload, off);
    off += 4;
    const bool required = (flags & 1u) != 0;
    const bool ignorable = (flags & 2u) != 0;
    if ((flags & ~3u) != 0 || (required && ignorable)) {
      return RejectStartup("08P01",
                           "SBWP.EXTENSION.REQUIRED_UNSUPPORTED",
                           "startup extension flags are invalid");
    }
    if (required) {
      return RejectStartup("08P01",
                           "SBWP.EXTENSION.UNKNOWN_REQUIRED",
                           "startup required an extension with no selected registry row");
    }
  }
  if (off != payload.size()) {
    return RejectStartup("08P01",
                         "NATIVE_WIRE.CONNECT_INVALID_PAYLOAD",
                         "startup payload contains trailing bytes");
  }
  return negotiated;
}

std::array<std::uint8_t, 16> TextToUuidBytes(std::string_view text) {
  std::array<std::uint8_t, 16> out{};
  auto hex_value = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  std::size_t nibble = 0;
  for (const char ch : text) {
    if (ch == '-') continue;
    const int value = hex_value(ch);
    if (value < 0 || nibble >= 32) return {};
    if ((nibble % 2) == 0) {
      out[nibble / 2] = static_cast<std::uint8_t>(value << 4);
    } else {
      out[nibble / 2] = static_cast<std::uint8_t>(out[nibble / 2] | value);
    }
    ++nibble;
  }
  return nibble == 32 ? out : std::array<std::uint8_t, 16>{};
}

bool IsZeroUuid(const std::array<std::uint8_t, 16>& uuid) {
  return std::all_of(uuid.begin(), uuid.end(), [](std::uint8_t value) { return value == 0; });
}

bool IsKnownSbwpMessage(std::uint8_t msg_type) {
  switch (msg_type) {
    case kStartup:
    case kAuthResponse:
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
    case kTxnStatus:
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

bool FeatureNegotiated(const SbwpSessionState& state, std::uint64_t feature) {
  if (feature == 0) return true;
  if (feature == ~0ull) return false;
  return (state.negotiated_features & feature) == feature;
}

std::array<std::uint8_t, 16> FallbackAttachmentId(std::string_view seed) {
  std::array<std::uint8_t, 16> out{};
  std::uint64_t hash = 1469598103934665603ull;
  for (unsigned char ch : seed) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::uint8_t>((hash >> ((i % 8u) * 8u)) & 0xffu);
  }
  out[6] = static_cast<std::uint8_t>((out[6] & 0x0fu) | 0x70u);
  out[8] = static_cast<std::uint8_t>((out[8] & 0x3fu) | 0x80u);
  return out;
}

std::string UuidKey(const std::array<std::uint8_t, 16>& uuid) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (const auto byte : uuid) {
    out.push_back(kHex[(byte >> 4u) & 0x0fu]);
    out.push_back(kHex[byte & 0x0fu]);
  }
  return out;
}

std::array<std::uint8_t, 16> GeneratedFinalityToken(
    std::uint64_t original_txn_id,
    std::uint32_t server_sequence,
    const std::array<std::uint8_t, 16>& idempotency_key) {
  return FallbackAttachmentId("sbwp-finality:" + std::to_string(original_txn_id) + ":" +
                              std::to_string(server_sequence) + ":" + UuidKey(idempotency_key));
}

bool ParseTxnCommitPayload(const std::vector<std::uint8_t>& payload,
                           SbwpTxnCommitRequest* request) {
  if (request == nullptr || payload.size() < 4) return false;
  *request = SbwpTxnCommitRequest{};
  request->legacy_flags = payload[0];
  if (payload.size() == 4 && payload[1] == 0 && payload[2] == 0 && payload[3] == 0) {
    return true;
  }
  if (payload.size() < 36 || payload[1] != kTxnFinalityPayloadVersion) return false;
  request->contract_flags = ReadU16(payload, 2);
  std::copy(payload.begin() + 4, payload.begin() + 20, request->idempotency_key.begin());
  request->request_fingerprint = ReadU64(payload, 20);
  request->expected_txn_id = ReadU64(payload, 28);
  return true;
}

bool ParseTxnFinalityQueryPayload(const std::vector<std::uint8_t>& payload,
                                  SbwpTxnFinalityQuery* query) {
  if (query == nullptr || payload.size() < 44 || payload[0] != kTxnFinalityPayloadVersion) {
    return false;
  }
  *query = SbwpTxnFinalityQuery{};
  query->flags = ReadU16(payload, 2);
  std::copy(payload.begin() + 4, payload.begin() + 20, query->idempotency_key.begin());
  std::copy(payload.begin() + 20, payload.begin() + 36, query->finality_token.begin());
  query->expected_txn_id = ReadU64(payload, 36);
  return true;
}

std::vector<std::uint8_t> TxnFinalityStatusPayload(const SbwpTxnFinalityRecord& record) {
  std::vector<std::uint8_t> out;
  out.reserve(64 + record.diagnostic_code.size() + record.detail.size());
  out.push_back(kTxnFinalityPayloadVersion);
  out.push_back(static_cast<std::uint8_t>(record.state));
  PutU16(&out, record.flags);
  PutUuid(&out, record.idempotency_key);
  PutUuid(&out, record.finality_token);
  PutU64(&out, record.request_fingerprint);
  PutU64(&out, record.original_txn_id);
  PutU64(&out, record.replacement_txn_id);
  PutU16Str(&out, record.diagnostic_code);
  PutU16Str(&out, record.detail);
  return out;
}

void StoreFinalityRecord(SbwpSessionState* state, SbwpTxnFinalityRecord record) {
  if (state == nullptr || IsZeroUuid(record.idempotency_key)) return;
  const std::string idempotency_key = UuidKey(record.idempotency_key);
  if (!IsZeroUuid(record.finality_token)) {
    state->idempotency_key_by_finality_token[UuidKey(record.finality_token)] = idempotency_key;
  }
  state->finality_by_idempotency_key[idempotency_key] = std::move(record);
}

const SbwpTxnFinalityRecord* FindFinalityRecord(
    const SbwpSessionState& state,
    const std::array<std::uint8_t, 16>& idempotency_key,
    const std::array<std::uint8_t, 16>& finality_token) {
  if (!IsZeroUuid(idempotency_key)) {
    const auto found = state.finality_by_idempotency_key.find(UuidKey(idempotency_key));
    if (found != state.finality_by_idempotency_key.end()) return &found->second;
  }
  if (!IsZeroUuid(finality_token)) {
    const auto token = state.idempotency_key_by_finality_token.find(UuidKey(finality_token));
    if (token != state.idempotency_key_by_finality_token.end()) {
      const auto found = state.finality_by_idempotency_key.find(token->second);
      if (found != state.finality_by_idempotency_key.end()) return &found->second;
    }
  }
  return nullptr;
}

SbwpTxnFinalityRecord NotFoundFinalityRecord(const SbwpTxnFinalityQuery& query) {
  SbwpTxnFinalityRecord record;
  record.state = SbwpTxnFinalityState::kNotFound;
  record.flags = kTxnFinalityFlagEngineKnown | kTxnFinalityFlagRetryRefused;
  record.idempotency_key = query.idempotency_key;
  record.finality_token = query.finality_token;
  record.original_txn_id = query.expected_txn_id;
  record.diagnostic_code = "SBWP.COMMIT.FINALITY_NOT_FOUND";
  record.detail = "no commit finality record for requested idempotency key or finality token";
  return record;
}

SbwpTxnFinalityRecord RefusedFinalityRecord(const SbwpTxnCommitRequest& request,
                                            const SbwpTxnFinalityRecord& existing,
                                            std::string diagnostic_code,
                                            std::string detail,
                                            bool side_effect_refused) {
  SbwpTxnFinalityRecord record = existing;
  record.state = SbwpTxnFinalityState::kRefused;
  record.flags &= ~kTxnFinalityFlagRetryAllowed;
  record.flags |= kTxnFinalityFlagEngineKnown | kTxnFinalityFlagRetryRefused;
  if (side_effect_refused) {
    record.flags |= kTxnFinalityFlagSideEffectRetryRefused;
  }
  record.idempotency_key = request.idempotency_key;
  record.request_fingerprint = request.request_fingerprint;
  record.diagnostic_code = std::move(diagnostic_code);
  record.detail = std::move(detail);
  return record;
}

bool SameCommitFingerprint(const SbwpTxnCommitRequest& request,
                           const SbwpTxnFinalityRecord& record) {
  if (request.request_fingerprint != 0 && record.request_fingerprint != 0 &&
      request.request_fingerprint != record.request_fingerprint) {
    return false;
  }
  if (request.expected_txn_id != 0 && record.original_txn_id != 0 &&
      request.expected_txn_id != record.original_txn_id) {
    return false;
  }
  return true;
}

bool IsPostInventorySecondaryFailure(const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    if (diagnostic.code == "SBWP.COMMIT.POST_INVENTORY_SECONDARY_FAILURE" ||
        diagnostic.code == "SB-IPAR-COMMIT-POST-INVENTORY-SECONDARY-FAILURE") {
      return true;
    }
  }
  return false;
}

std::optional<std::uint64_t> TextLineU64(std::string_view encoded, std::string_view key) {
  const std::string prefix = std::string(key) + "=";
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const std::size_t end = encoded.find('\n', start);
    const std::string_view line =
        encoded.substr(start, end == std::string_view::npos ? encoded.size() - start : end - start);
    if (line.starts_with(prefix)) {
      std::uint64_t value = 0;
      const auto text = line.substr(prefix.size());
      const auto* begin = text.data();
      const auto* finish = text.data() + text.size();
      const auto [ptr, ec] = std::from_chars(begin, finish, value);
      if (ec == std::errc() && ptr == finish) return value;
      return std::nullopt;
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return std::nullopt;
}

std::optional<std::uint64_t> TransactionIdFromResultPayload(std::string_view encoded) {
  if (const auto replacement = TextLineU64(encoded, "replacement_local_transaction_id")) {
    return replacement;
  }
  return TextLineU64(encoded, "local_transaction_id");
}

std::string FirstDiagnosticText(const MessageVectorSet& messages) {
  if (messages.diagnostics.empty()) return "operation rejected";
  const auto& diagnostic = messages.diagnostics.front();
  if (!diagnostic.message.empty()) return diagnostic.message;
  if (!diagnostic.code.empty()) return diagnostic.code;
  return "operation rejected";
}

std::string FirstDiagnosticCode(const MessageVectorSet& messages, std::string fallback) {
  if (!messages.diagnostics.empty() && !messages.diagnostics.front().code.empty()) {
    return messages.diagnostics.front().code;
  }
  return fallback;
}

std::string DiagnosticFieldValue(const MessageVectorSet& messages, std::string_view field_name) {
  for (const auto& diagnostic : messages.diagnostics) {
    for (const auto& field : diagnostic.fields) {
      if (field.name == field_name) return field.value;
    }
  }
  return {};
}

bool LooksInteger(std::string_view value) {
  if (value.empty()) return false;
  std::size_t i = (value[0] == '-' || value[0] == '+') ? 1 : 0;
  if (i == value.size()) return false;
  for (; i < value.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(value[i]))) return false;
  }
  return true;
}

bool LooksDecimal(std::string_view value) {
  bool saw_digit = false;
  bool saw_dot = false;
  std::size_t i = (!value.empty() && (value[0] == '-' || value[0] == '+')) ? 1 : 0;
  for (; i < value.size(); ++i) {
    const char ch = value[i];
    if (std::isdigit(static_cast<unsigned char>(ch))) {
      saw_digit = true;
      continue;
    }
    if (ch == '.' && !saw_dot) {
      saw_dot = true;
      continue;
    }
    return false;
  }
  return saw_digit && saw_dot;
}

std::uint32_t InferTypeOid(std::string_view value) {
  const auto upper = Upper(std::string(value));
  if (upper == "TRUE" || upper == "FALSE" || upper == "T" || upper == "F") return kOidBool;
  if (LooksInteger(value)) return kOidInt8;
  if (LooksDecimal(value)) return kOidNumeric;
  return kOidText;
}

std::vector<std::pair<std::string, std::string>> ParseFieldList(std::string_view body) {
  std::vector<std::pair<std::string, std::string>> fields;
  std::size_t start = 0;
  while (start <= body.size()) {
    const std::size_t end = body.find(';', start);
    const std::string_view field =
        body.substr(start, end == std::string_view::npos ? body.size() - start : end - start);
    const std::size_t eq = field.find('=');
    if (eq != std::string_view::npos) {
      fields.emplace_back(std::string(field.substr(0, eq)), std::string(field.substr(eq + 1)));
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return fields;
}

std::optional<std::vector<CopyImportRow>> ParseTextCopyRows(
    const std::vector<std::uint8_t>& payload) {
  std::vector<CopyImportRow> rows;
  std::istringstream in{
      std::string(reinterpret_cast<const char*>(payload.data()), payload.size())};
  std::string line;
  while (std::getline(in, line)) {
    line = Trim(std::move(line));
    if (line.empty()) continue;
    const auto parsed = ParseFieldList(line);
    if (parsed.empty()) return std::nullopt;
    CopyImportRow row;
    for (const auto& [name, value] : parsed) {
      if (name.empty()) return std::nullopt;
      const auto upper_value = Upper(value);
      if (upper_value == "NULL") {
        row.fields.push_back({name, std::nullopt});
      } else {
        row.fields.push_back({name, value});
      }
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::string HexEncodeCopyText(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2u);
  for (const unsigned char ch : value) {
    out.push_back(kHex[(ch >> 4u) & 0x0fu]);
    out.push_back(kHex[ch & 0x0fu]);
  }
  return out;
}

std::string CompactCopyRowsPayload(const CopyImportState& copy,
                                   std::size_t first_row,
                                   std::size_t end_row,
                                   bool include_field_names = true) {
  std::string out;
  for (std::size_t row_index = first_row; row_index < end_row; ++row_index) {
    const auto& row = copy.rows[row_index];
    for (const auto& [name, value] : row.fields) {
      if (!out.empty()) out.push_back(';');
      if (include_field_names) {
        out += HexEncodeCopyText(name);
      }
      out.push_back('|');
      out += HexEncodeCopyText(value.has_value() ? "text" : "null");
      out.push_back('|');
      out += HexEncodeCopyText(value.value_or(std::string{}));
      out.push_back('|');
      out += value.has_value() ? '0' : '1';
    }
  }
  return out;
}

bool CopyRowsHaveSharedShape(const CopyImportState& copy,
                             std::size_t first_row,
                             std::size_t end_row) {
  if (first_row >= end_row || end_row > copy.rows.size()) return false;
  const std::size_t column_count = copy.rows[first_row].fields.size();
  if (column_count == 0) return false;
  for (std::size_t row_index = first_row; row_index < end_row; ++row_index) {
    const auto& row = copy.rows[row_index];
    if (row.fields.size() != column_count) return false;
    for (std::size_t column_index = 0; column_index < column_count; ++column_index) {
      if (row.fields[column_index].first !=
          copy.rows[first_row].fields[column_index].first) {
        return false;
      }
    }
  }
  return true;
}

enum class NativeRowColumnType : std::uint8_t {
  kText = 1,
  kInt64 = 2,
  kBoolean = 3,
  kInt32 = 4,
  kUInt64 = 5,
  kReal64 = 6,
  kBinary = 7,
};

std::optional<std::int64_t> ParseLosslessNativeInt64(std::string_view value) {
  if (value.empty()) return std::nullopt;
  std::int64_t parsed = 0;
  const auto* begin = value.data();
  const auto* end = begin + value.size();
  const auto result = std::from_chars(begin, end, parsed, 10);
  if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
  if (std::to_string(parsed) != value) return std::nullopt;
  return parsed;
}

std::optional<std::int32_t> ParseLosslessNativeInt32(std::string_view value) {
  const auto parsed = ParseLosslessNativeInt64(value);
  if (!parsed.has_value() ||
      *parsed < std::numeric_limits<std::int32_t>::min() ||
      *parsed > std::numeric_limits<std::int32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::int32_t>(*parsed);
}

std::optional<std::uint64_t> ParseLosslessNativeUInt64(std::string_view value) {
  if (value.empty() || value.front() == '-') return std::nullopt;
  std::uint64_t parsed = 0;
  const auto* begin = value.data();
  const auto* end = begin + value.size();
  const auto result = std::from_chars(begin, end, parsed, 10);
  if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
  if (std::to_string(parsed) != value) return std::nullopt;
  return parsed;
}

bool NativeAsciiEqualIgnoreCase(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (std::tolower(static_cast<unsigned char>(left[index])) !=
        std::tolower(static_cast<unsigned char>(right[index]))) {
      return false;
    }
  }
  return true;
}

std::optional<bool> ParseLosslessNativeBool(std::string_view value) {
  if (NativeAsciiEqualIgnoreCase(value, "true")) return true;
  if (NativeAsciiEqualIgnoreCase(value, "false")) return false;
  return std::nullopt;
}

std::optional<double> ParseLosslessNativeReal64(std::string_view value) {
  if (value.empty() || value.find_first_of(".eE") == std::string_view::npos) {
    return std::nullopt;
  }
  double parsed = 0.0;
  const auto* begin = value.data();
  const auto* end = begin + value.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(parsed)) {
    return std::nullopt;
  }
  return parsed;
}

std::vector<NativeRowColumnType> InferNativeRowColumnTypes(const CopyImportState& copy,
                                                          std::size_t first_row,
                                                          std::size_t end_row) {
  std::vector<NativeRowColumnType> types;
  if (first_row >= end_row || end_row > copy.rows.size()) return types;
  const std::size_t column_count = copy.rows[first_row].fields.size();
  types.assign(column_count, NativeRowColumnType::kText);
  for (std::size_t column_index = 0; column_index < column_count; ++column_index) {
    bool saw_non_null = false;
    bool all_bool = true;
    bool all_int32 = true;
    bool all_lossless_int64 = true;
    bool all_uint64 = true;
    bool all_real64 = true;
	    for (std::size_t row_index = first_row; row_index < end_row; ++row_index) {
	      const auto& value = copy.rows[row_index].fields[column_index].second;
	      if (!value.has_value()) continue;
	      saw_non_null = true;
	      if (all_bool && !ParseLosslessNativeBool(*value).has_value()) {
	        all_bool = false;
	      }
	      if (all_int32 || all_lossless_int64) {
	        const auto parsed_i64 = ParseLosslessNativeInt64(*value);
	        if (!parsed_i64.has_value()) {
	          all_int32 = false;
	          all_lossless_int64 = false;
	        } else if (*parsed_i64 < std::numeric_limits<std::int32_t>::min() ||
	                   *parsed_i64 > std::numeric_limits<std::int32_t>::max()) {
	          all_int32 = false;
	        }
	      }
	      if (all_uint64 && !ParseLosslessNativeUInt64(*value).has_value()) {
	        all_uint64 = false;
	      }
	      if (all_real64 && !ParseLosslessNativeReal64(*value).has_value()) {
	        all_real64 = false;
	      }
	      if (!all_bool && !all_int32 && !all_lossless_int64 &&
	          !all_uint64 && !all_real64) {
	        break;
	      }
	    }
    if (!saw_non_null) {
      continue;
    }
    if (all_bool) {
      types[column_index] = NativeRowColumnType::kBoolean;
    } else if (all_int32) {
      types[column_index] = NativeRowColumnType::kInt32;
    } else if (all_lossless_int64) {
      types[column_index] = NativeRowColumnType::kInt64;
    } else if (all_uint64) {
      types[column_index] = NativeRowColumnType::kUInt64;
    } else if (all_real64) {
      types[column_index] = NativeRowColumnType::kReal64;
    }
  }
  return types;
}

void PutI64(std::vector<std::uint8_t>* out, std::int64_t value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(value));
  PutU64(out, bits);
}

void PutDouble(std::vector<std::uint8_t>* out, double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(value));
  PutU64(out, bits);
}

bool NativeRowTypeSupported(NativeRowColumnType type) {
  switch (type) {
    case NativeRowColumnType::kText:
    case NativeRowColumnType::kInt64:
    case NativeRowColumnType::kBoolean:
    case NativeRowColumnType::kInt32:
    case NativeRowColumnType::kUInt64:
    case NativeRowColumnType::kReal64:
    case NativeRowColumnType::kBinary:
      return true;
  }
  return false;
}

std::vector<std::uint8_t> BuildNativeRowPacket(const CopyImportState& copy,
                                               std::size_t first_row,
                                               std::size_t row_count) {
  const std::size_t end_row = std::min(copy.rows.size(), first_row + row_count);
  std::vector<std::uint8_t> out;
  if (!CopyRowsHaveSharedShape(copy, first_row, end_row)) return out;
  const auto& columns = copy.rows[first_row].fields;
  const auto column_types = InferNativeRowColumnTypes(copy, first_row, end_row);
  if (column_types.size() != columns.size()) return out;
  const std::size_t null_bitmap_bytes = (columns.size() + 7u) / 8u;
  out.reserve(20 + columns.size() * 18 + (end_row - first_row) *
                                          (null_bitmap_bytes + columns.size() * 8));
  out.push_back('S');
  out.push_back('B');
  out.push_back('N');
  out.push_back('R');
  PutU16(&out, 2);
  PutU16(&out, 0);
  PutU64(&out, static_cast<std::uint64_t>(end_row - first_row));
  PutU32(&out, static_cast<std::uint32_t>(columns.size()));
  for (const auto type : column_types) {
    out.push_back(static_cast<std::uint8_t>(type));
  }
  for (const auto& [name, _] : columns) {
    PutU32(&out, static_cast<std::uint32_t>(name.size()));
    out.insert(out.end(), name.begin(), name.end());
  }
  for (std::size_t row_index = first_row; row_index < end_row; ++row_index) {
    const auto& row = copy.rows[row_index];
    std::vector<std::uint8_t> null_bitmap(null_bitmap_bytes, 0);
    for (std::size_t column_index = 0; column_index < row.fields.size(); ++column_index) {
      if (!row.fields[column_index].second.has_value()) {
        null_bitmap[column_index / 8u] |=
            static_cast<std::uint8_t>(1u << (column_index % 8u));
      }
    }
    out.insert(out.end(), null_bitmap.begin(), null_bitmap.end());
    for (std::size_t column_index = 0; column_index < row.fields.size(); ++column_index) {
      const auto& value = row.fields[column_index].second;
      if (!value.has_value()) continue;
      switch (column_types[column_index]) {
        case NativeRowColumnType::kBoolean: {
          const auto parsed = ParseLosslessNativeBool(*value);
          if (!parsed.has_value()) return {};
          out.push_back(*parsed ? 1u : 0u);
          break;
        }
        case NativeRowColumnType::kInt32: {
          const auto parsed = ParseLosslessNativeInt32(*value);
          if (!parsed.has_value()) return {};
          PutI32(&out, *parsed);
          break;
        }
        case NativeRowColumnType::kInt64: {
          const auto parsed = ParseLosslessNativeInt64(*value);
          if (!parsed.has_value()) return {};
          PutI64(&out, *parsed);
          break;
        }
        case NativeRowColumnType::kUInt64: {
          const auto parsed = ParseLosslessNativeUInt64(*value);
          if (!parsed.has_value()) return {};
          PutU64(&out, *parsed);
          break;
        }
        case NativeRowColumnType::kReal64: {
          const auto parsed = ParseLosslessNativeReal64(*value);
          if (!parsed.has_value()) return {};
          PutDouble(&out, *parsed);
          break;
        }
        case NativeRowColumnType::kBinary:
        case NativeRowColumnType::kText:
        default:
          PutU32(&out, static_cast<std::uint32_t>(value->size()));
          out.insert(out.end(), value->begin(), value->end());
          break;
      }
    }
  }
  return out;
}

std::optional<NativeCopyPacket> ParseNativeRowCopyPacketHeader(
    const std::vector<std::uint8_t>& payload) {
  if (payload.size() < 20 ||
      payload[0] != 'S' || payload[1] != 'B' || payload[2] != 'N' || payload[3] != 'R') {
    return std::nullopt;
  }
  std::size_t off = 4;
  const std::uint16_t version = ReadU16(payload, off);
  off += 2;
  if (version != 2) return std::nullopt;
  off += 2;  // flags
  NativeCopyPacket packet;
  packet.row_count = ReadU64(payload, off);
  off += 8;
  const std::uint32_t column_count = ReadU32(payload, off);
  off += 4;
  if (packet.row_count == 0 || column_count == 0 || off + column_count > payload.size()) {
    return std::nullopt;
  }
  std::vector<NativeRowColumnType> column_types;
  column_types.reserve(column_count);
  packet.column_type_tags.reserve(column_count);
  for (std::uint32_t i = 0; i < column_count; ++i) {
    const auto type = static_cast<NativeRowColumnType>(payload[off++]);
    if (!NativeRowTypeSupported(type)) {
      return std::nullopt;
    }
    column_types.push_back(type);
    packet.column_type_tags.push_back(static_cast<std::uint8_t>(type));
  }
  packet.columns.reserve(column_count);
  for (std::uint32_t i = 0; i < column_count; ++i) {
    if (off + 4 > payload.size()) return std::nullopt;
    const std::uint32_t name_size = ReadU32(payload, off);
    off += 4;
    if (name_size == 0 || off + name_size > payload.size()) return std::nullopt;
    packet.columns.emplace_back(reinterpret_cast<const char*>(payload.data() + off), name_size);
    off += name_size;
  }
  const std::size_t null_bitmap_bytes = (static_cast<std::size_t>(column_count) + 7u) / 8u;
  for (std::uint64_t row_index = 0; row_index < packet.row_count; ++row_index) {
    if (off + null_bitmap_bytes > payload.size()) return std::nullopt;
    const std::size_t null_bitmap_offset = off;
    off += null_bitmap_bytes;
    for (std::uint32_t column_index = 0; column_index < column_count; ++column_index) {
      const bool is_null =
          (payload[null_bitmap_offset + column_index / 8u] &
           static_cast<std::uint8_t>(1u << (column_index % 8u))) != 0;
      if (is_null) continue;
      switch (column_types[column_index]) {
        case NativeRowColumnType::kBoolean:
          if (off + 1 > payload.size()) return std::nullopt;
          off += 1;
          break;
        case NativeRowColumnType::kInt32:
          if (off + 4 > payload.size()) return std::nullopt;
          off += 4;
          break;
        case NativeRowColumnType::kInt64:
          if (off + 8 > payload.size()) return std::nullopt;
          off += 8;
          break;
        case NativeRowColumnType::kUInt64:
        case NativeRowColumnType::kReal64:
          if (off + 8 > payload.size()) return std::nullopt;
          off += 8;
          break;
        case NativeRowColumnType::kBinary:
        case NativeRowColumnType::kText:
        default: {
          if (off + 4 > payload.size()) return std::nullopt;
          const std::uint32_t value_size = ReadU32(payload, off);
          off += 4;
          if (off + value_size > payload.size()) return std::nullopt;
          off += value_size;
          break;
        }
      }
    }
  }
  if (off != payload.size()) return std::nullopt;
  packet.payload = payload;
  return packet;
}

std::optional<std::vector<CopyImportRow>> ParseNativeRowCopyRows(
    const std::vector<std::uint8_t>& payload) {
  if (payload.size() < 20 ||
      payload[0] != 'S' || payload[1] != 'B' || payload[2] != 'N' || payload[3] != 'R') {
    return std::nullopt;
  }
  std::size_t off = 4;
  const std::uint16_t version = ReadU16(payload, off);
  off += 2;
  if (version != 2) return std::nullopt;
  off += 2;  // flags
  const std::uint64_t row_count_u64 = ReadU64(payload, off);
  off += 8;
  if (row_count_u64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::nullopt;
  }
  const std::uint32_t column_count = ReadU32(payload, off);
  off += 4;
  if (column_count == 0 || off + column_count > payload.size()) return std::nullopt;
  std::vector<NativeRowColumnType> column_types;
  column_types.reserve(column_count);
  for (std::uint32_t i = 0; i < column_count; ++i) {
    const auto type = static_cast<NativeRowColumnType>(payload[off++]);
    if (!NativeRowTypeSupported(type)) {
      return std::nullopt;
    }
    column_types.push_back(type);
  }
  std::vector<std::string> names;
  names.reserve(column_count);
  for (std::uint32_t i = 0; i < column_count; ++i) {
    if (off + 4 > payload.size()) return std::nullopt;
    const std::uint32_t name_size = ReadU32(payload, off);
    off += 4;
    if (name_size == 0 || off + name_size > payload.size()) return std::nullopt;
    names.emplace_back(reinterpret_cast<const char*>(payload.data() + off), name_size);
    off += name_size;
  }
  const std::size_t row_count = static_cast<std::size_t>(row_count_u64);
  const std::size_t null_bitmap_bytes = (static_cast<std::size_t>(column_count) + 7u) / 8u;
  std::vector<CopyImportRow> rows;
  rows.reserve(row_count);
  for (std::size_t row_index = 0; row_index < row_count; ++row_index) {
    if (off + null_bitmap_bytes > payload.size()) return std::nullopt;
    const std::size_t null_bitmap_offset = off;
    off += null_bitmap_bytes;
    CopyImportRow row;
    row.fields.reserve(column_count);
    for (std::uint32_t column_index = 0; column_index < column_count; ++column_index) {
      const bool is_null =
          (payload[null_bitmap_offset + column_index / 8u] &
           static_cast<std::uint8_t>(1u << (column_index % 8u))) != 0;
      std::optional<std::string> value;
      if (!is_null) {
        switch (column_types[column_index]) {
          case NativeRowColumnType::kBoolean: {
            if (off + 1 > payload.size()) return std::nullopt;
            value = payload[off++] == 0 ? "false" : "true";
            break;
          }
          case NativeRowColumnType::kInt32: {
            if (off + 4 > payload.size()) return std::nullopt;
            const std::uint32_t bits = ReadU32(payload, off);
            off += 4;
            std::int32_t parsed = 0;
            std::memcpy(&parsed, &bits, sizeof(parsed));
            value = std::to_string(parsed);
            break;
          }
          case NativeRowColumnType::kInt64: {
            if (off + 8 > payload.size()) return std::nullopt;
            std::uint64_t bits = ReadU64(payload, off);
            off += 8;
            std::int64_t parsed = 0;
            std::memcpy(&parsed, &bits, sizeof(parsed));
            value = std::to_string(parsed);
            break;
          }
          case NativeRowColumnType::kUInt64: {
            if (off + 8 > payload.size()) return std::nullopt;
            value = std::to_string(ReadU64(payload, off));
            off += 8;
            break;
          }
          case NativeRowColumnType::kReal64: {
            if (off + 8 > payload.size()) return std::nullopt;
            const std::uint64_t bits = ReadU64(payload, off);
            off += 8;
            double parsed = 0.0;
            std::memcpy(&parsed, &bits, sizeof(parsed));
            value = std::to_string(parsed);
            break;
          }
          case NativeRowColumnType::kBinary:
          case NativeRowColumnType::kText:
          default: {
            if (off + 4 > payload.size()) return std::nullopt;
            const std::uint32_t value_size = ReadU32(payload, off);
            off += 4;
            if (off + value_size > payload.size()) return std::nullopt;
            value.emplace(reinterpret_cast<const char*>(payload.data() + off), value_size);
            off += value_size;
            break;
          }
        }
      }
      row.fields.emplace_back(names[column_index], std::move(value));
    }
    rows.push_back(std::move(row));
  }
  if (off != payload.size()) return std::nullopt;
  return rows;
}

std::optional<std::vector<CopyImportRow>> ParseBinaryCopyRows(
    const std::vector<std::uint8_t>& payload) {
  if (payload.size() >= 4 &&
      payload[0] == 'S' && payload[1] == 'B' && payload[2] == 'N' && payload[3] == 'R') {
    return ParseNativeRowCopyRows(payload);
  }
  if (payload.size() < 12 ||
      payload[0] != 'S' || payload[1] != 'B' || payload[2] != 'C' || payload[3] != 'P' ||
      payload[4] != 1) {
    return std::nullopt;
  }
  std::size_t off = 8;
  const std::uint32_t row_count = ReadU32(payload, off);
  off += 4;
  std::vector<CopyImportRow> rows;
  rows.reserve(row_count);
  for (std::uint32_t row_index = 0; row_index < row_count; ++row_index) {
    if (off + 4 > payload.size()) return std::nullopt;
    const std::uint32_t field_count = ReadU32(payload, off);
    off += 4;
    CopyImportRow row;
    row.fields.reserve(field_count);
    for (std::uint32_t field_index = 0; field_index < field_count; ++field_index) {
      if (off + 4 > payload.size()) return std::nullopt;
      const std::uint32_t name_size = ReadU32(payload, off);
      off += 4;
      if (off + name_size + 1u + 4u > payload.size()) return std::nullopt;
      std::string name(reinterpret_cast<const char*>(payload.data() + off), name_size);
      off += name_size;
      const bool is_null = payload[off++] != 0;
      const std::uint32_t value_size = ReadU32(payload, off);
      off += 4;
      if (off + value_size > payload.size()) return std::nullopt;
      std::optional<std::string> value;
      if (!is_null) {
        value.emplace(reinterpret_cast<const char*>(payload.data() + off), value_size);
      }
      off += value_size;
      if (name.empty()) return std::nullopt;
      row.fields.emplace_back(std::move(name), std::move(value));
    }
    rows.push_back(std::move(row));
  }
  if (off != payload.size()) return std::nullopt;
  return rows;
}

std::string GenerateCopyImportRowUuid() {
  static std::atomic<std::uint64_t> sequence{0};
  const auto now_millis = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const auto generated = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::row,
      now_millis + sequence.fetch_add(1, std::memory_order_relaxed));
  return generated.ok() ? uuid::UuidToString(generated.value.value) : std::string{};
}

std::string EscapeOperationOperandField(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (char ch : value) {
    if (ch == '\\' || ch == '\t' || ch == '\n' || ch == '\r') out.push_back('\\');
    if (ch == '\n') {
      out.push_back('n');
    } else if (ch == '\r') {
      out.push_back('r');
    } else if (ch == '\t') {
      out.push_back('t');
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

std::string BuildCopyExecuteEnvelope(const CopyImportState& copy,
                                     std::size_t first_row,
                                     std::size_t row_count) {
  const std::size_t end_row = std::min(copy.rows.size(), first_row + row_count);
  std::string out;
  out += "operation_id=dml.execute_import_rows\n";
  out += "opcode=SBLR_DML_EXECUTE_IMPORT_ROWS\n";
  out += "sblr_operation_family=sblr.dml.operation.v3\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=sbsql.sbwp.copy_data.execute_import\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += "requires_transaction_context=true\n";
  out += "requires_cluster_authority=false\n";
  out += "target_object_uuid=" + copy.target_object_uuid + "\n";
  out += "target_object_kind=table\n";
  out += "dml_surface_variant=copy_import_export\n";
  out += "source_kind=csv_stream\n";
  out += "format_family=csv\n";
  out += "source_fingerprint=sbwp-copy-data-canonical-row-fields\n";
  out += "source_position=row:0\n";
  out += "encoding=utf8\n";
  out += "line_ending=lf\n";
  out += "delimiter=,\n";
  out += "quote=\"\n";
  out += "escape=\"\n";
  out += "header_policy=absent\n";
  out += "estimated_row_count=" + std::to_string(end_row - first_row) + "\n";
  out += "insert_mode=copy_import\n";
  out += "reject_mode=reject_row\n";
  out += "reject_limit_rows=16\n";
  out += "reject_payload_policy=diagnostic_only\n";
  out += "resume_policy=fail_closed\n";
  out += "checkpoint_mode=disabled\n";
  out += "duplicate_mode=error\n";
  out += "require_generated_row_uuid=true\n";
  out += "strict_bulk_load_requested=false\n";
  out += "operand=text\tfeature.strict_bulk_load\tenabled\n";
  out += "operand=text\tmemory.spill_allowed\ttrue\n";
  out += "operand=text\tmemory.context_budget_bytes\t67108864\n";
  out += "operand=text\tmemory.bulk_load_budget_bytes\t67108864\n";
  out += "operand=text\tcopy_append_batch_rows\t" + std::to_string(end_row - first_row) + "\n";
  if (copy.source_size_bytes != 0) {
    out += "operand=text\tcopy.source_size_bytes\t" +
           std::to_string(copy.source_size_bytes) + "\n";
  }
  if (copy.preallocation_bytes != 0) {
    out += "operand=text\tcopy.preallocation_bytes\t" +
           std::to_string(copy.preallocation_bytes) + "\n";
  }
  if (copy.preallocation_factor_percent != 0) {
    out += "operand=text\tcopy.preallocation_factor_percent\t" +
           std::to_string(copy.preallocation_factor_percent) + "\n";
  }
  for (std::size_t row_index = first_row; row_index < end_row; ++row_index) {
    const auto& row = copy.rows[row_index];
    const std::string row_uuid = GenerateCopyImportRowUuid();
    for (const auto& [name, value] : row.fields) {
      out += value.has_value() ? "operand=row_field\t" : "operand=row_null_field\t";
      out += row_uuid;
      out += "|";
      out += EscapeOperationOperandField(name);
      out += "\t";
      out += value.has_value() ? EscapeOperationOperandField(*value) : "";
      out += "\n";
    }
  }
  return out;
}

std::string BuildCopyExecuteEnvelope(const CopyImportState& copy) {
  return BuildCopyExecuteEnvelope(copy, 0, copy.rows.size());
}

std::string BuildNativeBulkIngestExecuteEnvelope(const CopyImportState& copy,
                                                std::size_t first_row,
                                                std::size_t row_count,
                                                bool include_compact_payload = true) {
  const std::size_t end_row = std::min(copy.rows.size(), first_row + row_count);
  std::string out;
  out += "operation_id=dml.execute_native_bulk_ingest\n";
  out += "opcode=SBLR_DML_EXECUTE_NATIVE_BULK_INGEST\n";
  out += "sblr_operation_family=sblr.dml.operation.v3\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=cdp041-sb_isql-native-bulk-ingest\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += "requires_transaction_context=true\n";
  out += "requires_cluster_authority=false\n";
  out += "target_object_uuid=" + copy.target_object_uuid + "\n";
  out += "target_object_kind=table\n";
  out += "dml_surface_variant=sb_isql_native_bulk_ingest\n";
  out += "source_kind=binary_typed_rows\n";
  out += "format_family=binary_typed_rows\n";
  out += "source_fingerprint=sb-isql-native-bulk-ingest-canonical-row-fields\n";
  out += "source_position=row:0\n";
  out += "estimated_row_count=" + std::to_string(end_row - first_row) + "\n";
  out += "native_bulk_ingest=true\n";
  out += std::string("native_bulk_ingest_enabled=") +
         (copy.native_bulk_ingest_enabled ? "true\n" : "false\n");
  out += "reject_mode=fail_fast\n";
  out += "reject_limit_rows=0\n";
  out += "reject_payload_policy=diagnostic_only\n";
  out += "result_payload_policy=summary_only\n";
  out += "resume_policy=fail_closed\n";
  out += "checkpoint_mode=disabled\n";
  out += "duplicate_mode=error\n";
  out += "require_generated_row_uuid=true\n";
  out += "operand=text\tphysical_mga_cow\tfalse\n";
  out += "operand=text\tinsert_trace.rows\tfalse\n";
  out += "operand=text\tsblr.rowset_default_markers_absent\ttrue\n";
  if (copy.source_size_bytes != 0) {
    out += "operand=text\tcopy.source_size_bytes\t";
    out += std::to_string(copy.source_size_bytes);
    out += "\n";
  }
  if (copy.preallocation_bytes != 0) {
    out += "operand=text\tcopy.preallocation_bytes\t";
    out += std::to_string(copy.preallocation_bytes);
    out += "\n";
  }
  if (copy.preallocation_factor_percent != 0) {
    out += "operand=text\tcopy.preallocation_factor_percent\t";
    out += std::to_string(copy.preallocation_factor_percent);
    out += "\n";
  }
  if (end_row > first_row) {
    const std::size_t column_count = copy.rows[first_row].fields.size();
    const bool shared_shape = CopyRowsHaveSharedShape(copy, first_row, end_row);
    if (shared_shape) {
      out += "operand=text\tinsert_values_row_count\t";
      out += std::to_string(end_row - first_row);
      out += "\n";
      out += "operand=text\tinsert_values_column_count\t";
      out += std::to_string(column_count);
      out += "\n";
      out += "operand=text\tinsert_values_column_list_present\tfalse\n";
      if (include_compact_payload) {
        out += "operand=text\tinsert_values_compact_format\tsbsql.insert_values.cells.v1\n";
        out += "operand=text\tinsert_values_compact_payload\t";
        out += EscapeOperationOperandField(
            CompactCopyRowsPayload(copy, first_row, end_row, false));
        out += "\n";
      } else {
        out += "operand=text\tnative_row_packet_required\ttrue\n";
        out += "operand=text\tnative_row_packet_format\tscratchbird.native_rows.v2\n";
      }
      out += "operand=text\tinsert_values_parser_executes_sql\tfalse\n";
      for (std::size_t column_index = 0; column_index < column_count; ++column_index) {
        out += "operand=text\tinsert_values_descriptor_column_";
        out += std::to_string(column_index);
        out += "\t";
        out += EscapeOperationOperandField(copy.rows[first_row].fields[column_index].first);
        out += "\n";
      }
      out += "operand=text\tsblr.canonical_rowset_shared_shape\ttrue\n";
    } else {
      for (std::size_t row_index = first_row; row_index < end_row; ++row_index) {
        const auto& row = copy.rows[row_index];
        const std::string row_uuid = GenerateCopyImportRowUuid();
        for (const auto& [name, value] : row.fields) {
          out += value.has_value() ? "operand=row_field:character\t"
                                   : "operand=row_null_field:character\t";
          out += row_uuid;
          out += "|";
          out += EscapeOperationOperandField(name);
          out += "\t";
          out += value.has_value() ? EscapeOperationOperandField(*value) : "";
          out += "\n";
        }
      }
    }
  }
  return out;
}

std::string BuildNativeBulkIngestExecuteEnvelopeForPacket(
    const CopyImportState& copy,
    const NativeCopyPacket& packet) {
  std::string out;
  out += "operation_id=dml.execute_native_bulk_ingest\n";
  out += "opcode=SBLR_DML_EXECUTE_NATIVE_BULK_INGEST\n";
  out += "sblr_operation_family=sblr.dml.operation.v3\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=cdp041-sb_isql-native-bulk-ingest\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += "requires_transaction_context=true\n";
  out += "requires_cluster_authority=false\n";
  out += "target_object_uuid=" + copy.target_object_uuid + "\n";
  out += "target_object_kind=table\n";
  out += "dml_surface_variant=sb_isql_native_bulk_ingest\n";
  out += "source_kind=binary_typed_rows\n";
  out += "format_family=binary_typed_rows\n";
  out += "source_fingerprint=sb-isql-native-bulk-ingest-native-row-packet\n";
  out += "source_position=row:0\n";
  out += "estimated_row_count=" + std::to_string(packet.row_count) + "\n";
  out += "native_bulk_ingest=true\n";
  out += std::string("native_bulk_ingest_enabled=") +
         (copy.native_bulk_ingest_enabled ? "true\n" : "false\n");
  out += "reject_mode=fail_fast\n";
  out += "reject_limit_rows=0\n";
  out += "reject_payload_policy=diagnostic_only\n";
  out += "result_payload_policy=summary_only\n";
  out += "resume_policy=fail_closed\n";
  out += "checkpoint_mode=disabled\n";
  out += "duplicate_mode=error\n";
  out += "require_generated_row_uuid=true\n";
  out += "operand=text\tphysical_mga_cow\tfalse\n";
  out += "operand=text\tinsert_trace.rows\tfalse\n";
  if (copy.source_size_bytes != 0) {
    out += "operand=text\tcopy.source_size_bytes\t";
    out += std::to_string(copy.source_size_bytes);
    out += "\n";
  }
  if (copy.preallocation_bytes != 0) {
    out += "operand=text\tcopy.preallocation_bytes\t";
    out += std::to_string(copy.preallocation_bytes);
    out += "\n";
  }
  if (copy.preallocation_factor_percent != 0) {
    out += "operand=text\tcopy.preallocation_factor_percent\t";
    out += std::to_string(copy.preallocation_factor_percent);
    out += "\n";
  }
  out += "operand=text\tinsert_values_row_count\t";
  out += std::to_string(packet.row_count);
  out += "\n";
  out += "operand=text\tinsert_values_column_count\t";
  out += std::to_string(packet.columns.size());
  out += "\n";
  out += "operand=text\tinsert_values_column_list_present\tfalse\n";
  out += "operand=text\tnative_row_packet_required\ttrue\n";
  out += "operand=text\tnative_row_packet_format\tscratchbird.native_rows.v2\n";
  out += "operand=text\tinsert_values_parser_executes_sql\tfalse\n";
  for (std::size_t column_index = 0; column_index < packet.columns.size(); ++column_index) {
    out += "operand=text\tinsert_values_descriptor_column_";
    out += std::to_string(column_index);
    out += "\t";
    out += EscapeOperationOperandField(packet.columns[column_index]);
    out += "\n";
  }
  out += "operand=text\tsblr.canonical_rowset_shared_shape\ttrue\n";
  out += "operand=text\tsblr.rowset_default_markers_absent\ttrue\n";
  return out;
}

std::string NativeCopyPacketDescriptorFingerprint(const NativeCopyPacket& packet) {
  std::string out;
  out.reserve(packet.columns.size() * 24u);
  out += "sbnr.v2:";
  out += std::to_string(packet.columns.size());
  for (std::size_t index = 0; index < packet.columns.size(); ++index) {
    out.push_back('|');
    out += std::to_string(index);
    out.push_back(':');
    out += index < packet.column_type_tags.size()
               ? std::to_string(static_cast<unsigned>(packet.column_type_tags[index]))
               : "missing_type";
    out.push_back(':');
    out += std::to_string(packet.columns[index].size());
    out.push_back(':');
    out += packet.columns[index];
  }
  return out;
}

bool BindOrValidateNativeCopyDescriptor(CopyImportState* copy,
                                        const NativeCopyPacket& packet,
                                        std::string* diagnostic_code,
                                        std::string* diagnostic_detail) {
  if (copy == nullptr) {
    if (diagnostic_code != nullptr) {
      *diagnostic_code = "SBSQL.COPY.DESCRIPTOR_CONTEXT_MISSING";
    }
    if (diagnostic_detail != nullptr) {
      *diagnostic_detail = "COPY native descriptor validation requires parser COPY state";
    }
    return false;
  }
  if (packet.columns.empty() ||
      packet.column_type_tags.size() != packet.columns.size()) {
    if (diagnostic_code != nullptr) {
      *diagnostic_code = "SBSQL.COPY.DESCRIPTOR_INVALID";
    }
    if (diagnostic_detail != nullptr) {
      *diagnostic_detail = "COPY native row packet descriptor is missing columns or type tags";
    }
    return false;
  }
  const std::string fingerprint = NativeCopyPacketDescriptorFingerprint(packet);
  if (!copy->native_packet_descriptor_bound) {
    copy->native_packet_descriptor_bound = true;
    copy->native_packet_descriptor_fingerprint = fingerprint;
    copy->native_packet_descriptor_column_count =
        static_cast<std::uint32_t>(packet.columns.size());
    return true;
  }
  if (copy->native_packet_descriptor_fingerprint != fingerprint ||
      copy->native_packet_descriptor_column_count !=
          static_cast<std::uint32_t>(packet.columns.size())) {
    if (diagnostic_code != nullptr) {
      *diagnostic_code = "SBSQL.COPY.DESCRIPTOR_MISMATCH";
    }
    if (diagnostic_detail != nullptr) {
      *diagnostic_detail =
          "COPY native row packet descriptor changed after the prepared bulk stream was bound";
    }
    return false;
  }
  return true;
}

bool PrepareCopyNativeBulkHandle(SbsqlTestWireSession* session,
                                 SbwpSessionState* state,
                                 CopyImportState* copy,
                                 std::string* diagnostic_code,
                                 std::string* diagnostic_detail) {
  if (diagnostic_code != nullptr) diagnostic_code->clear();
  if (diagnostic_detail != nullptr) diagnostic_detail->clear();
  if (session == nullptr || state == nullptr || copy == nullptr) {
    if (diagnostic_code != nullptr) {
      *diagnostic_code = "SBSQL.COPY.PREPARED_HANDLE_CONTEXT_MISSING";
    }
    if (diagnostic_detail != nullptr) {
      *diagnostic_detail = "COPY native bulk prepare requires parser session state";
    }
    return false;
  }
  if (!copy->native_bulk_ingest) {
    return true;
  }
  RefreshWireAuthorityEpochsFromSession(*session, state);
  CopyImportState prepare_template;
  prepare_template.native_bulk_ingest = true;
  prepare_template.native_bulk_ingest_enabled = copy->native_bulk_ingest_enabled;
  prepare_template.sql = copy->sql;
  prepare_template.target_object_uuid = copy->target_object_uuid;
  prepare_template.source_size_bytes = copy->source_size_bytes;
  prepare_template.preallocation_bytes = copy->preallocation_bytes;
  prepare_template.preallocation_factor_percent =
      copy->preallocation_factor_percent;
  const auto prepared = session->PrepareSblrForWire(
      BuildNativeBulkIngestExecuteEnvelope(prepare_template, 0, 0));
  RefreshWireAuthorityEpochsFromSession(*session, state);
  if (prepared.accepted && !prepared.prepared_statement_uuid.empty()) {
    copy->prepared_statement_uuid = prepared.prepared_statement_uuid;
    copy->prepared_operation_id = prepared.operation_id;
    CaptureCopyPreparedAuthorityEpochs(*session, copy);
    return true;
  }
  if (diagnostic_code != nullptr) {
    *diagnostic_code =
        FirstDiagnosticCode(prepared.messages,
                            "SBSQL.COPY.PREPARED_HANDLE_REFUSED");
  }
  if (diagnostic_detail != nullptr) {
    *diagnostic_detail = DiagnosticFieldValue(prepared.messages, "detail");
    if (diagnostic_detail->empty()) {
      *diagnostic_detail =
          "COPY native bulk path requires a server-owned prepared SBLR handle";
    }
  }
  return false;
}

std::uint32_t CopyWindowBytesForSession(const SbwpSessionState& state, std::uint8_t format) {
  const std::uint32_t base =
      format == kCopyFormatBinaryRowsetV1 ? kCopyBinaryWindowBytes : kCopyDefaultWindowBytes;
  if (const auto found = state.session_parameters.find("copy_window_bytes");
      found != state.session_parameters.end()) {
    try {
      const auto parsed = static_cast<std::uint64_t>(std::stoull(found->second));
      if (parsed >= 16u * 1024u && parsed <= 64u * 1024u * 1024u) {
        return static_cast<std::uint32_t>(parsed);
      }
    } catch (...) {
    }
  }
  return base;
}

std::uint64_t CopyU64ParameterForSession(const SbwpSessionState& state,
                                         std::initializer_list<const char*> keys,
                                         std::uint64_t fallback = 0) {
  for (const char* key : keys) {
    const auto found = state.session_parameters.find(key);
    if (found == state.session_parameters.end()) {
      continue;
    }
    try {
      const auto parsed = static_cast<std::uint64_t>(std::stoull(found->second));
      return parsed == 0 ? fallback : parsed;
    } catch (...) {
      return fallback;
    }
  }
  return fallback;
}

std::vector<std::uint8_t> CopyInResponsePayload(SbwpSessionState* state) {
  const bool binary_copy =
      state != nullptr && FeatureNegotiated(*state, kFeatureBinaryCopy);
  const std::uint8_t format =
      binary_copy ? kCopyFormatBinaryRowsetV1 : kCopyFormatCanonicalRowFieldsText;
  const std::uint32_t window_bytes =
      state == nullptr ? kCopyDefaultWindowBytes : CopyWindowBytesForSession(*state, format);
  if (state != nullptr) {
    state->copy_import.copy_data_format = format;
    state->copy_import.window_bytes = window_bytes;
    state->copy_import.format_family =
        binary_copy ? "binary_rowset_v1" : "canonical_row_fields";
    state->copy_import.source_size_bytes =
        CopyU64ParameterForSession(*state,
                                   {"copy.source_size_bytes",
                                    "copy_source_size_bytes",
                                    "dml.ingest.source_size_bytes"});
    state->copy_import.preallocation_bytes =
        CopyU64ParameterForSession(*state,
                                   {"copy.preallocation_bytes",
                                    "copy_preallocation_bytes",
                                    "dml.ingest.preallocation_bytes"});
    state->copy_import.preallocation_factor_percent =
        CopyU64ParameterForSession(*state,
                                   {"copy.preallocation_factor_percent",
                                    "copy_preallocation_factor_percent",
                                    "dml.ingest.preallocation_factor_percent"},
                                   82);
    state->session_parameters.erase("copy.source_size_bytes");
    state->session_parameters.erase("copy_source_size_bytes");
    state->session_parameters.erase("dml.ingest.source_size_bytes");
    state->session_parameters.erase("copy.preallocation_bytes");
    state->session_parameters.erase("copy_preallocation_bytes");
    state->session_parameters.erase("dml.ingest.preallocation_bytes");
    state->session_parameters.erase("copy.preallocation_factor_percent");
    state->session_parameters.erase("copy_preallocation_factor_percent");
    state->session_parameters.erase("dml.ingest.preallocation_factor_percent");
  }
  std::vector<std::uint8_t> out;
  out.push_back(format);
  PutU32(&out, window_bytes);
  return out;
}

std::optional<std::string> JsonObjectTextField(std::string_view object, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\":\"";
  const auto start = object.find(needle);
  if (start == std::string_view::npos) return std::nullopt;
  std::size_t cursor = start + needle.size();
  std::string out;
  while (cursor < object.size()) {
    const char ch = object[cursor++];
    if (ch == '"') return out;
    if (ch == '\\' && cursor < object.size()) {
      out.push_back(object[cursor++]);
      continue;
    }
    out.push_back(ch);
  }
  return std::nullopt;
}

std::optional<std::string> JsonObjectU64Field(std::string_view object, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\":";
  const auto start = object.find(needle);
  if (start == std::string_view::npos) return std::nullopt;
  std::size_t cursor = start + needle.size();
  while (cursor < object.size() && std::isspace(static_cast<unsigned char>(object[cursor]))) ++cursor;
  const std::size_t begin = cursor;
  while (cursor < object.size() && std::isdigit(static_cast<unsigned char>(object[cursor]))) ++cursor;
  if (cursor == begin) return std::nullopt;
  return std::string(object.substr(begin, cursor - begin));
}

RowSet ParseSyntheticJsonRowsFromResultPayload(std::string_view payload) {
  RowSet rowset;
  const auto rows_start = payload.find("\"rows\":[");
  if (rows_start == std::string_view::npos) return rowset;
  rowset.columns.push_back({"operation_id", kOidText, -1, -1, 0, false});
  rowset.columns.push_back({"row_index", kOidInt8, 8, -1, 0, false});
  rowset.columns.push_back({"status", kOidText, -1, -1, 0, false});

  std::size_t cursor = rows_start;
  while (true) {
    const auto object_start = payload.find("{\"operation_id\"", cursor);
    if (object_start == std::string_view::npos) break;
    const auto object_end = payload.find('}', object_start);
    if (object_end == std::string_view::npos) break;
    const auto object = payload.substr(object_start, object_end - object_start + 1);
    const auto operation_id = JsonObjectTextField(object, "operation_id");
    const auto row_index = JsonObjectU64Field(object, "row_index");
    const auto status = JsonObjectTextField(object, "status");
    if (operation_id && row_index && status) {
      rowset.rows.push_back({*operation_id, *row_index, *status});
    }
    cursor = object_end + 1;
  }
  if (rowset.rows.empty()) { rowset.columns.clear(); }
  return rowset;
}

RowSet ParseRowsFromResultPayload(std::string_view payload) {
  RowSet rowset;
  std::map<std::string, std::size_t> column_index;
  std::istringstream in{std::string(payload)};
  std::string line;
  while (std::getline(in, line)) {
    if (!line.starts_with("row[")) continue;
    const auto eq = line.find("]=");
    if (eq == std::string::npos) continue;
    const auto fields = ParseFieldList(std::string_view(line).substr(eq + 2));
    if (fields.empty()) continue;
    std::vector<std::optional<std::string>> row(rowset.columns.size());
    for (const auto& [name, value] : fields) {
      auto found = column_index.find(name);
      if (found == column_index.end()) {
        const auto ordinal = rowset.columns.size();
        column_index[name] = ordinal;
        SbwpColumn column;
        column.name = name.empty() ? "column" + std::to_string(ordinal + 1) : name;
        column.type_oid = InferTypeOid(value);
        rowset.columns.push_back(std::move(column));
        for (auto& existing : rowset.rows) existing.resize(rowset.columns.size());
        row.resize(rowset.columns.size());
        found = column_index.find(name);
      }
      row[found->second] = value;
    }
    rowset.rows.push_back(std::move(row));
  }
  if (rowset.rows.empty()) {
    return ParseSyntheticJsonRowsFromResultPayload(payload);
  }
  return rowset;
}

class ClientIo {
 public:
  explicit ClientIo(std::intptr_t fd) : fd_(fd) {}
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
    SSL_set_fd(ssl_, static_cast<int>(fd_));
    return SSL_accept(ssl_) == 1;
  }

  bool ReadExact(std::uint8_t* data, std::size_t size) {
    std::size_t got = 0;
    while (got < size) {
      int rc = 0;
      if (ssl_ != nullptr) {
        rc = SSL_read(ssl_, data + got, static_cast<int>(size - got));
        if (rc <= 0) {
          const int err = SSL_get_error(ssl_, rc);
          if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
          return false;
        }
      } else {
#ifndef _WIN32
        const ssize_t raw = ::read(static_cast<int>(fd_), data + got, size - got);
        if (raw < 0 && errno == EINTR) continue;
        if (raw <= 0) return false;
        rc = static_cast<int>(raw);
#else
        const int want = static_cast<int>(std::min<std::size_t>(
            size - got, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        rc = ::recv(static_cast<SOCKET>(fd_), reinterpret_cast<char*>(data + got), want, 0);
        if (rc <= 0) return false;
#endif
      }
      got += static_cast<std::size_t>(rc);
    }
    return true;
  }

  bool WriteAll(const std::uint8_t* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
      int rc = 0;
      if (ssl_ != nullptr) {
        rc = SSL_write(ssl_, data + written, static_cast<int>(size - written));
        if (rc <= 0) {
          const int err = SSL_get_error(ssl_, rc);
          if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
          return false;
        }
      } else {
#ifndef _WIN32
        const ssize_t raw = ::write(static_cast<int>(fd_), data + written, size - written);
        if (raw < 0 && errno == EINTR) continue;
        if (raw <= 0) return false;
        rc = static_cast<int>(raw);
#else
        const int want = static_cast<int>(std::min<std::size_t>(
            size - written, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        rc = ::send(static_cast<SOCKET>(fd_), reinterpret_cast<const char*>(data + written), want, 0);
        if (rc <= 0) return false;
#endif
      }
      written += static_cast<std::size_t>(rc);
    }
    return true;
  }

 private:
  std::intptr_t fd_{-1};
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
    frame->header.txn_id |=
        static_cast<std::uint64_t>(header[32 + static_cast<std::size_t>(shift / 8)]) << shift;
  }
  frame->payload.assign(frame->header.length, 0);
  return frame->payload.empty() || io->ReadExact(frame->payload.data(), frame->payload.size());
}

bool SendFrame(ClientIo* io,
               SbwpSessionState* state,
               std::uint8_t msg_type,
               const std::vector<std::uint8_t>& payload) {
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

std::vector<std::uint8_t> AuthRequestPayload() {
  return {1, 0, 0, 0};
}

std::vector<std::uint8_t> AuthOkPayload(const SbwpSessionState& state) {
  std::vector<std::uint8_t> out;
  out.insert(out.end(), state.attachment_id.begin(), state.attachment_id.end());
  PutU32(&out, 0);
  return out;
}

std::vector<std::uint8_t> ReadyPayload(const SbwpSessionState& state, ReadyReason reason) {
  if (state.p1_payloads) {
    std::vector<std::uint8_t> out;
    out.reserve(76);
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
  PutU64(&out, state.txn_id == 0 ? 0 : state.txn_id);
  return out;
}

bool SendReady(ClientIo* io,
               SbwpSessionState* state,
               ReadyReason reason = ReadyReason::kCommandComplete) {
  state->ready_sent_for_current_operation = true;
  return SendFrame(io, state, kReady, ReadyPayload(*state, reason));
}

void PutParameterStatusKv(std::vector<std::uint8_t>* out,
                          std::string_view key,
                          std::string_view value,
                          bool defaulted = false) {
  PutLpStr(out, key);
  out->push_back(0x01);
  out->push_back(0);
  out->push_back(defaulted ? 1 : 0);
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
                         SbwpSessionState* state,
                         const std::vector<std::pair<std::string, std::string>>& values) {
  if (!state->p1_payloads || values.empty()) return true;
  return SendFrame(io, state, kParameterStatus, ParameterStatusPayload(values));
}

std::vector<std::uint8_t> ServerInfoPayload(const SbwpSessionState& state,
                                            const ParserConfig& config) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, state.session_uuid);
  PutZeroUuid(&out);
  PutZeroUuid(&out);
  PutLpStr(&out, "ScratchBird native SBWP P1");
  PutLpStr(&out, "public-alpha");
  PutLpStr(&out, config.dialect.empty() ? "sbsql" : config.dialect);
  PutU64(&out, 1);
  PutU64(&out, kServerSupportedFeatureMask);
  PutU64(&out, 1);
  return out;
}

bool SendServerInfo(ClientIo* io, SbwpSessionState* state, const ParserConfig& config) {
  if (!state->p1_payloads) return true;
  return SendFrame(io, state, kServerInfo, ServerInfoPayload(*state, config));
}

std::vector<std::pair<std::string, std::string>> StartupParameterStatuses(
    const SbwpSessionState& state) {
  std::vector<std::pair<std::string, std::string>> values;
  values.push_back({"protocol.selected_version",
                    state.selected_protocol_version == kSbwpVersionCurrent ? "1.1" : "1.0"});
  values.push_back({"protocol.negotiated_features", std::to_string(state.negotiated_features)});
  if (!state.authenticated_user_uuid.empty()) {
    values.push_back({"session.authenticated_user_uuid", state.authenticated_user_uuid});
  }
  if (!state.auth_provider_family.empty()) {
    values.push_back({"session.auth_provider_family", state.auth_provider_family});
  }
  if (!state.principal_claim.empty()) {
    values.push_back({"session.principal_claim", state.principal_claim});
  }
  if (state.security_policy_epoch != 0) {
    values.push_back({"security.generation", std::to_string(state.security_policy_epoch)});
  }
  if (!state.language_profile.empty()) {
    values.push_back({"language.profile_id", state.language_profile});
  }
  if (!state.language_tag.empty()) {
    values.push_back({"language.tag", state.language_tag});
  }
  if (!state.default_language_tag.empty()) {
    values.push_back({"language.default_tag", state.default_language_tag});
  }
  if (!state.input_syntax_profile.empty()) {
    values.push_back({"language.input_syntax_profile", state.input_syntax_profile});
  }
  if (!state.input_language_fallback_tag.empty()) {
    values.push_back({"language.input_fallback_tag",
                      state.input_language_fallback_tag});
  }
  if (!state.common_resource_hash.empty()) {
    values.push_back({"language.common_resource_hash", state.common_resource_hash});
  }
  if (state.language_resource_epoch != 0) {
    values.push_back({"language.resource_epoch",
                      std::to_string(state.language_resource_epoch)});
  }
  if (state.localized_name_epoch != 0) {
    values.push_back({"language.localized_name_epoch",
                      std::to_string(state.localized_name_epoch)});
  }
  if (state.message_resource_epoch != 0) {
    values.push_back({"language.message_epoch",
                      std::to_string(state.message_resource_epoch)});
  }
  if (!state.resource_compatibility_identity.empty()) {
    values.push_back({"language.resource_compatibility_identity",
                      state.resource_compatibility_identity});
  }
  if (!state.resource_version_identity.empty()) {
    values.push_back({"language.resource_version_identity",
                      state.resource_version_identity});
  }
  if (state.txn_id != 0) {
    values.push_back({"current_txn_id", std::to_string(state.txn_id)});
  }
  if (state.snapshot_visible_through_local_transaction_id != 0) {
    values.push_back({"session.snapshot_visible_through_local_transaction_id",
                      std::to_string(state.snapshot_visible_through_local_transaction_id)});
  }
  for (const auto& key : {"client_encoding", "charset", "timezone", "time_zone",
                          "application_name", "default_schema", "default_catalog"}) {
    const auto found = state.session_parameters.find(key);
    if (found != state.session_parameters.end()) values.push_back({key, found->second});
  }
  return values;
}

std::vector<std::uint8_t> ErrorPayload(std::string_view sqlstate,
                                       std::string_view message,
                                       std::string_view detail = {}) {
  std::vector<std::uint8_t> out;
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

std::vector<std::uint8_t> CommandCompletePayload(std::uint64_t rows, std::string_view tag);

bool SendError(ClientIo* io,
               SbwpSessionState* state,
               std::string_view sqlstate,
               std::string_view message,
               std::string_view detail = {}) {
  return SendFrame(io, state, kError, ErrorPayload(sqlstate, message, detail));
}

bool SendFeatureNotNegotiated(ClientIo* io, SbwpSessionState* state, std::uint8_t msg_type) {
  return SendError(io,
                   state,
                   "08P01",
                   "SBWP.FEATURE.NOT_NEGOTIATED",
                   "frame 0x" + std::to_string(msg_type) +
                       " requires a feature that was not negotiated for this session");
}

bool SendUnsupportedFeature(ClientIo* io,
                            SbwpSessionState* state,
                            std::string_view feature_name) {
  return SendError(io,
                   state,
                   "0A000",
                   "FEATURE.NOT_IMPLEMENTED_RELEASE_BLOCKING",
                   std::string(feature_name) + " is fail-closed before side effects in this route");
}

bool SendTxnFinalityStatus(ClientIo* io,
                           SbwpSessionState* state,
                           const SbwpTxnFinalityRecord& record) {
  return SendFrame(io, state, kTxnStatus, TxnFinalityStatusPayload(record));
}

bool SendTxnCommitReplayRefusal(ClientIo* io,
                                SbwpSessionState* state,
                                const SbwpTxnCommitRequest& request,
                                const SbwpTxnFinalityRecord& existing,
                                std::string diagnostic_code,
                                std::string detail,
                                bool side_effect_refused) {
  const auto record = RefusedFinalityRecord(request,
                                           existing,
                                           std::move(diagnostic_code),
                                           std::move(detail),
                                           side_effect_refused);
  return SendTxnFinalityStatus(io, state, record) &&
         SendError(io, state, "40003", record.diagnostic_code, record.detail) &&
         SendReady(io, state, ReadyReason::kErrorRecovered);
}

bool HandleTxnStatus(ClientIo* io, SbwpSessionState* state, const Frame& frame) {
  SbwpTxnFinalityQuery query;
  if (!ParseTxnFinalityQueryPayload(frame.payload, &query)) {
    return SendError(io,
                     state,
                     "08P01",
                     "SBWP.COMMIT.FINALITY_QUERY_INVALID",
                     "TxnStatus query payload is truncated or has an unsupported version") &&
           SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  const auto* record = FindFinalityRecord(*state, query.idempotency_key, query.finality_token);
  const SbwpTxnFinalityRecord not_found = NotFoundFinalityRecord(query);
  return SendTxnFinalityStatus(io, state, record == nullptr ? not_found : *record) &&
         SendReady(io, state, ReadyReason::kCommandComplete);
}

bool HandleTxnCommitReplay(ClientIo* io,
                           SbwpSessionState* state,
                           const SbwpTxnCommitRequest& request) {
  if ((request.contract_flags & kTxnCommitFlagHasIdempotencyKey) == 0 ||
      IsZeroUuid(request.idempotency_key)) {
    return false;
  }
  const auto* existing = FindFinalityRecord(*state, request.idempotency_key, {});
  if (existing == nullptr) return false;
  if (!SameCommitFingerprint(request, *existing)) {
    return SendTxnCommitReplayRefusal(
        io,
        state,
        request,
        *existing,
        "SBWP.RETRY.IDEMPOTENCY_KEY_CONFLICT",
        "idempotency key is already bound to a different commit request fingerprint",
        false);
  }
  if ((existing->flags & kTxnFinalityFlagEngineKnown) == 0) {
    return SendTxnFinalityStatus(io, state, *existing) &&
           SendError(io,
                     state,
                     "08007",
                     "SERVER.DRIVER_TX.RETRY_REQUIRES_FINALITY_QUERY",
                     "commit finality is unknown until engine finality is queried") &&
           SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  const bool side_effect_retry =
      (request.contract_flags & kTxnCommitFlagStatementHasSideEffects) != 0;
  const bool caller_acknowledged =
      (request.contract_flags & kTxnCommitFlagCallerAcknowledgedRetryBoundary) != 0;
  if (side_effect_retry && !caller_acknowledged) {
    return SendTxnCommitReplayRefusal(
        io,
        state,
        request,
        *existing,
        "SBWP.RETRY.CALLER_ACK_REQUIRED",
        "side-effect commit retry refused without caller acknowledgement of the retry boundary",
        true);
  }
  if (existing->replacement_txn_id != 0) {
    state->txn_id = existing->replacement_txn_id;
  }
  return SendTxnFinalityStatus(io, state, *existing) &&
         SendFrame(io, state, kCommandComplete, CommandCompletePayload(0, "COMMIT")) &&
         SendReady(io, state, ReadyReason::kCommandComplete);
}

std::array<std::uint8_t, 16> PayloadUuidOrGenerated(const std::vector<std::uint8_t>& payload) {
  std::array<std::uint8_t, 16> uuid{};
  if (payload.size() >= uuid.size()) {
    std::copy(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(uuid.size()), uuid.begin());
    if (!IsZeroUuid(uuid)) return uuid;
  }
  return FallbackAttachmentId("cancel-request");
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

bool HandleCancel(ClientIo* io, SbwpSessionState* state, const Frame& frame) {
  if (!state->authenticated) {
    return SendError(io,
                     state,
                     "28000",
                     "SECURITY.CANCEL_NOT_AUTHORIZED",
                     "cancel requires an authenticated owner session");
  }
  const auto cancel_uuid = PayloadUuidOrGenerated(frame.payload);
  if (!SendFrame(io, state, kCancelAck, CancelAckPayload(cancel_uuid))) return false;
  const bool target_supplied = frame.payload.size() >= 32 &&
                               !std::all_of(frame.payload.begin() + 16,
                                            frame.payload.begin() + 32,
                                            [](std::uint8_t byte) { return byte == 0; });
  const auto payload = target_supplied
                           ? CancelledPayload(cancel_uuid,
                                              "user_statement_cancelled",
                                              "57014",
                                              "EXECUTION.QUERY_CANCELLED",
                                              "statement_aborted_transaction_state_reported_by_engine",
                                              "application_decision")
                           : CancelledPayload(cancel_uuid,
                                              "cancel_target_not_found",
                                              "57014",
                                              "EXECUTION.CANCEL_TARGET_NOT_FOUND",
                                              "no_known_state_change",
                                              "no");
  if (!SendFrame(io, state, kCancelled, payload)) return false;
  return SendReady(io, state, ReadyReason::kCancelOutcome);
}

bool RollbackForReset(SbsqlTestWireSession* session, SbwpSessionState* state, std::string* detail) {
  if (state->txn_id == 0) return true;
  const auto rollback = session->RunPipeline("ROLLBACK", true);
  if (!rollback.accepted || rollback.messages.has_errors()) {
    *detail = FirstDiagnosticText(rollback.messages);
    return false;
  }
  const auto replacement = TransactionIdFromResultPayload(rollback.server_result_payload);
  if (!replacement.has_value() || *replacement == 0) {
    *detail = "replacement_transaction_missing";
    return false;
  }
  state->txn_id = *replacement;
  return true;
}

bool HandleResetSession(SbsqlTestWireSession* session,
                        ClientIo* io,
                        SbwpSessionState* state,
                        const Frame& frame) {
  if (!FeatureNegotiated(*state, kFeatureSessionReset)) {
    return SendFeatureNotNegotiated(io, state, frame.header.msg_type);
  }
  if (!state->authenticated) {
    return SendError(io, state, "28000", "authentication required");
  }
  const bool rollback_open_transaction = frame.payload.size() > 17 && frame.payload[17] != 0;
  if (state->txn_id != 0 && !rollback_open_transaction) {
    if (!SendError(io,
                   state,
                   "25001",
                   "NATIVE_WIRE.RESET_REFUSED_OPEN_TRANSACTION",
                   "reset_session requires rollback_open_transaction when a transaction is open")) {
      return false;
    }
    return SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  std::string rollback_detail;
  if (!RollbackForReset(session, state, &rollback_detail)) {
    if (!SendError(io,
                   state,
                   "08006",
                   "SESSION.RESET_TIMEOUT",
                   rollback_detail.empty() ? "reset rollback did not complete" : rollback_detail)) {
      return false;
    }
    return SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  if (frame.payload.size() <= 18 || frame.payload[18] != 0) {
    state->statements.clear();
    state->portals.clear();
  }
  if (frame.payload.size() <= 20 || frame.payload[20] != 0) {
    state->session_parameters.erase("traceparent");
    state->session_parameters.erase("tracestate");
  }
  if (!SendParameterStatus(io,
                           state,
                           {{"session.reset", "complete"},
                            {"protocol.selected_version",
                             state->selected_protocol_version == kSbwpVersionCurrent ? "1.1" : "1.0"}})) {
    return false;
  }
  return SendReady(io, state, ReadyReason::kResetComplete);
}

bool HandleReauth(ClientIo* io, SbwpSessionState* state, const Frame& frame) {
  if (!FeatureNegotiated(*state, kFeatureReauth)) {
    return SendFeatureNotNegotiated(io, state, frame.header.msg_type);
  }
  if (!state->authenticated || frame.payload.empty()) {
    if (!SendError(io,
                   state,
                   "28000",
                   frame.payload.empty() ? "SECURITY.REAUTH_TIMEOUT"
                                         : "NATIVE_WIRE.REAUTH_REQUIRED",
                   "reauth requires a non-empty engine-verifiable auth payload")) {
      return false;
    }
    return SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  state->session_parameters["security.generation"] = "refreshed";
  if (!SendParameterStatus(io, state, {{"security.generation", "refreshed"}})) return false;
  return SendReady(io, state, ReadyReason::kReauthComplete);
}

bool ValidTraceparent(std::string_view traceparent) {
  if (traceparent.empty()) return true;
  if (traceparent.size() != 55) return false;
  return traceparent[2] == '-' && traceparent[35] == '-' && traceparent[52] == '-';
}

bool HandleTraceContext(ClientIo* io, SbwpSessionState* state, const Frame& frame) {
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
    if (!SendError(io,
                   state,
                   "08P01",
                   "NATIVE_WIRE.TRACE_CONTEXT_INVALID",
                   "trace context failed W3C shape or payload validation")) {
      return false;
    }
    return SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  state->session_parameters["traceparent"] = traceparent;
  state->session_parameters["tracestate"] = tracestate.empty() ? "present" : "redacted";
  return SendParameterStatus(io, state, {{"traceparent", traceparent.empty() ? "absent" : "present"}}) &&
         SendReady(io, state, ReadyReason::kStateChange);
}

bool HandleSubscription(ClientIo* io, SbwpSessionState* state, bool subscribe) {
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

std::vector<std::uint8_t> CommandCompletePayload(std::uint64_t rows, std::string_view tag) {
  std::vector<std::uint8_t> out;
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
  std::uint16_t family = 0;
  std::uint16_t code = 0;
  switch (oid) {
    case kOidBool:
      family = 1;
      code = 1;
      break;
    case kOidInt4:
      family = 2;
      code = 3;
      break;
    case kOidInt8:
      family = 2;
      code = 4;
      break;
    case kOidFloat8:
      family = 6;
      code = 2;
      break;
    case kOidNumeric:
      family = 4;
      code = 1;
      break;
    case kOidText:
    default:
      family = oid == 0 ? 0 : 8;
      code = oid == 0 ? 0 : 1;
      break;
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
  const auto count = static_cast<std::uint16_t>(
      std::min<std::size_t>(statement.param_types.size(), UINT16_MAX));
  PutU16(&out, count);
  PutU16(&out, 0);
  for (std::uint16_t i = 0; i < count; ++i) PutU32(&out, statement.param_types[i]);
  return out;
}

std::vector<std::uint8_t> RowDescriptionPayload(const std::vector<SbwpColumn>& columns,
                                                bool p1_payloads) {
  if (p1_payloads) {
    std::vector<std::uint8_t> out;
    PutU16(&out, 1);
    out.push_back(0);
    out.push_back(1);
    PutU32(&out, static_cast<std::uint32_t>(columns.size()));
    PutZeroUuid(&out);
    PutZeroUuid(&out);
    PutHash256Zero(&out);
    std::uint32_t ordinal = 1;
    for (const auto& column : columns) {
      PutU32(&out, ordinal++);
      out.push_back(0);
      out.push_back(column.format == 0 ? 1 : 0);
      out.push_back(column.nullable ? 1 : 0);
      out.push_back(0);
      const std::uint64_t metadata_bitmap = FeatureBit(0) | FeatureBit(10);
      PutU64(&out, metadata_bitmap);
      PutCanonicalTypeRef(&out, column.type_oid);
      PutZeroUuid(&out);
      PutZeroUuid(&out);
      PutZeroUuid(&out);
      PutU32(&out, 0);
      out.push_back(0);
      out.push_back(0);
      PutU16(&out, 0);
      PutNullableText(&out, column.name);
    }
    return out;
  }
  std::vector<std::uint8_t> out;
  PutU16(&out, static_cast<std::uint16_t>(std::min<std::size_t>(columns.size(), UINT16_MAX)));
  PutU16(&out, 0);
  std::uint16_t ordinal = 0;
  for (const auto& column : columns) {
    PutU32(&out, static_cast<std::uint32_t>(column.name.size()));
    out.insert(out.end(), column.name.begin(), column.name.end());
    PutU32(&out, 0);
    PutU16(&out, ordinal++);
    PutU32(&out, column.type_oid);
    PutI16(&out, column.type_size);
    PutI32(&out, column.type_modifier);
    out.push_back(column.format);
    out.push_back(column.nullable ? 1 : 0);
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

std::string CommandTagFor(std::string_view sql, const PipelineResult& result) {
  const auto normalized = Upper(StripSqlTerminator(std::string(sql)));
  const bool sql_surface_is_copy = normalized.starts_with("COPY");
  const auto completion_count =
      result.server_affected_rows_present ? result.server_affected_rows
                                          : result.server_row_count;
  if (!result.server_operation_id.empty()) {
    if (result.server_operation_id == "transaction.begin") return "BEGIN";
    if (result.server_operation_id == "transaction.commit") return "COMMIT";
    if (result.server_operation_id == "transaction.rollback") return "ROLLBACK";
    if (result.server_operation_id == "dml.execute_native_bulk_ingest") {
      if (sql_surface_is_copy) {
        return "COPY " + std::to_string(completion_count);
      }
      if (normalized.starts_with("INSERT")) {
        return "INSERT " + std::to_string(completion_count);
      }
      return "NATIVE_BULK_INGEST " + std::to_string(completion_count);
    }
  }
  std::string token;
  for (char ch : normalized) {
    if (std::isspace(static_cast<unsigned char>(ch)) || ch == '(') break;
    token.push_back(ch);
  }
  return token.empty() ? "COMMAND" : token;
}

std::uint64_t CommandCompleteRowsFor(const PipelineResult& result) {
  if (result.server_affected_rows_present) {
    return result.server_affected_rows;
  }
  return result.server_row_count;
}

bool ShouldAutoCursor(std::string_view sql) {
  return Upper(StripSqlTerminator(std::string(sql))).starts_with("SELECT");
}

void RefreshWireTransactionStateFromSession(const SbsqlTestWireSession& session,
                                            SbwpSessionState* state) {
  if (state == nullptr) return;
  RefreshWireAuthorityEpochsFromSession(session, state);
  const auto& context = session.session();
  if (context.local_transaction_id == 0) return;
  state->txn_id = context.local_transaction_id;
  state->snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
}

bool AdmitFrameTransaction(ClientIo* io,
                           SbwpSessionState* state,
                           const Frame& frame,
                           std::string_view operation) {
  if (state == nullptr || frame.header.txn_id == 0 || state->txn_id == 0 ||
      frame.header.txn_id == state->txn_id) {
    return true;
  }
  return SendError(io,
                   state,
                   "25000",
                   "SBWP.TRANSACTION_ID_MISMATCH",
                   std::string(operation) +
                       " frame transaction does not match the current engine transaction") &&
         SendReady(io, state, ReadyReason::kErrorRecovered);
}

bool NativeBulkIngestRequested(std::string_view sql) {
  const std::string upper = Upper(StripSqlTerminator(std::string(sql)));
  return upper.find("NATIVE_BULK_INGEST") != std::string::npos ||
         upper.find("NATIVE BULK INGEST") != std::string::npos;
}

bool NativeBulkIngestEnabledRequested(std::string_view sql) {
  const std::string upper = Upper(StripSqlTerminator(std::string(sql)));
  return upper.find("NATIVE_BULK_INGEST_ENABLED=FALSE") == std::string::npos &&
         upper.find("NATIVE_BULK_INGEST_ENABLED FALSE") == std::string::npos &&
         upper.find("NATIVE_BULK_INGEST_DISABLED") == std::string::npos &&
         upper.find(" DISABLED") == std::string::npos;
}

bool IsZeroUuidText(std::string_view value) {
  bool saw_hex_digit = false;
  for (char ch : value) {
    if (ch == '-') continue;
    if (ch != '0') return false;
    saw_hex_digit = true;
  }
  return saw_hex_digit;
}

bool SendPipelineResult(ClientIo* io,
                        SbsqlTestWireSession* session,
                        SbwpSessionState* state,
                        std::string_view sql,
                        const PipelineResult& result) {
  if (!result.server_cursor_uuid.empty() && !IsZeroUuidText(result.server_cursor_uuid) &&
      session != nullptr) {
    bool row_description_sent = false;
    std::uint64_t emitted_rows = 0;
    bool end_of_cursor = false;
    while (!end_of_cursor) {
      const auto fetched = session->FetchCursorOnRoute(result.server_cursor_uuid,
                                                       kAutoCursorFetchRows,
                                                       kAutoCursorFetchBytes);
      if (!fetched.accepted || fetched.messages.has_errors()) {
        (void)session->CancelCursorOnRoute(result.server_cursor_uuid);
        return SendError(io,
                         state,
                         "42000",
                         FirstDiagnosticText(fetched.messages),
                         FirstDiagnosticCode(fetched.messages,
                                             "PARSER_SERVER_IPC.FETCH_REJECTED"));
      }
      RowSet rowset = ParseRowsFromResultPayload(fetched.row_packet);
      if (!rowset.rows.empty()) {
        if (!row_description_sent) {
          if (!SendFrame(io,
                         state,
                         kRowDescription,
                         RowDescriptionPayload(rowset.columns, state->p1_payloads))) {
            (void)session->CancelCursorOnRoute(result.server_cursor_uuid);
            return false;
          }
          row_description_sent = true;
        }
        for (const auto& row : rowset.rows) {
          if (!SendFrame(io, state, kDataRow, DataRowPayload(row))) {
            (void)session->CancelCursorOnRoute(result.server_cursor_uuid);
            return false;
          }
        }
        emitted_rows += rowset.rows.size();
      }
      end_of_cursor = fetched.end_of_cursor;
      if (fetched.row_count == 0 && fetched.row_packet.empty() && !end_of_cursor) {
        (void)session->CancelCursorOnRoute(result.server_cursor_uuid);
        return SendError(io,
                         state,
                         "XX000",
                         "PARSER_SERVER_IPC.EMPTY_CURSOR_BATCH",
                         "cursor fetch returned no rows without reaching end of cursor");
      }
    }
    const auto closed = session->CloseCursorOnRoute(result.server_cursor_uuid);
    if (!closed.accepted || closed.messages.has_errors()) {
      return SendError(io,
                       state,
                       "42000",
                       FirstDiagnosticText(closed.messages),
                       FirstDiagnosticCode(closed.messages,
                                           "PARSER_SERVER_IPC.CLOSE_CURSOR_REJECTED"));
    }
    return SendFrame(io,
                     state,
                     kCommandComplete,
                     CommandCompletePayload(emitted_rows,
                                            "SELECT " + std::to_string(emitted_rows)));
  }
  RowSet rowset = ParseRowsFromResultPayload(result.server_result_payload);
  if (!rowset.rows.empty()) {
    if (!SendFrame(io, state, kRowDescription, RowDescriptionPayload(rowset.columns,
                                                                     state->p1_payloads))) {
      return false;
    }
    for (const auto& row : rowset.rows) {
      if (!SendFrame(io, state, kDataRow, DataRowPayload(row))) return false;
    }
    if (result.server_operation_id == "dml.execute_import_rows" ||
        result.server_operation_id == "dml.execute_native_bulk_ingest") {
      return SendFrame(io,
                       state,
                       kCommandComplete,
                       CommandCompletePayload(CommandCompleteRowsFor(result),
                                              CommandTagFor(sql, result)));
    }
    return SendFrame(io,
                     state,
                     kCommandComplete,
                     CommandCompletePayload(rowset.rows.size(),
                                            "SELECT " + std::to_string(rowset.rows.size())));
  }
  return SendFrame(io,
                   state,
                   kCommandComplete,
                   CommandCompletePayload(CommandCompleteRowsFor(result),
                                          CommandTagFor(sql, result)));
}

std::optional<bool> TryExecuteSimpleInsertRowsetFastPath(SbsqlTestWireSession* session,
                                                         ClientIo* io,
                                                         SbwpSessionState* state,
                                                         CopyImportState rowset,
                                                         bool send_ready,
                                                         bool* command_accepted) {
  const bool phase_trace = ParserPhaseTraceEnabled();
  const std::int64_t total_started = phase_trace ? ParserPhaseNowNs() : 0;
  const std::size_t row_count = rowset.rows.size();
  const std::size_t sql_bytes = rowset.sql.size();
  const auto write_total_trace = [&](std::string_view detail) {
    WriteParserPhaseTraceIfEnabled(phase_trace,
                                   "simple_insert_rowset_fast_path",
                                   "total",
                                   total_started,
                                   sql_bytes,
                                   1,
                                   row_count,
                                   detail);
  };
  state->ready_sent_for_current_operation = false;
  if (command_accepted != nullptr) *command_accepted = true;
  RefreshWireAuthorityEpochsFromSession(*session, state);
  const std::string presented_shape_key =
      SimpleInsertRowsetPresentedShapeKey(rowset);
  auto cache_it = state->simple_insert_rowset_cache.find(presented_shape_key);
  bool cache_hit = cache_it != state->simple_insert_rowset_cache.end() &&
                   SimpleInsertRowsetPreparedEntryCurrent(cache_it->second,
                                                          session->session());
  if (cache_hit) {
    rowset.target_object_uuid = cache_it->second.target_object_uuid;
    WriteParserPhaseTraceIfEnabled(phase_trace,
                                   "simple_insert_rowset_fast_path",
                                   "resolve_target_uuid",
                                   phase_trace ? ParserPhaseNowNs() : 0,
                                   rowset.target_name.size(),
                                   1,
                                   row_count,
                                   "prepared_shape_cache_hit");
  } else {
    const std::int64_t resolve_started = phase_trace ? ParserPhaseNowNs() : 0;
    auto resolved = session->ResolvePublicNameForWire(rowset.target_name, false, "relation");
    RefreshWireAuthorityEpochsFromSession(*session, state);
    WriteParserPhaseTraceIfEnabled(phase_trace,
                                   "simple_insert_rowset_fast_path",
                                   "resolve_target_uuid",
                                   resolve_started,
                                   rowset.target_name.size(),
                                   1,
                                   row_count,
                                   resolved.resolved ? "resolved" : "not_resolved");
    if (!resolved.resolved || resolved.object_uuid.empty()) {
      write_total_trace("not_applicable_unresolved_target");
      return std::nullopt;
    }

    rowset.target_object_uuid = std::move(resolved.object_uuid);
    cache_it = state->simple_insert_rowset_cache.find(presented_shape_key);
    cache_hit = cache_it != state->simple_insert_rowset_cache.end() &&
                SimpleInsertRowsetPreparedEntryCurrent(cache_it->second,
                                                       session->session()) &&
                cache_it->second.target_object_uuid == rowset.target_object_uuid;
  }
  const std::int64_t cache_lookup_started = phase_trace ? ParserPhaseNowNs() : 0;
  const std::string cache_key = SimpleInsertRowsetCacheKey(rowset);
  WriteParserPhaseTraceIfEnabled(phase_trace,
                                 "simple_insert_rowset_fast_path",
                                 "prepare_cache_lookup",
                                 cache_lookup_started,
                                 presented_shape_key.size() + cache_key.size(),
                                 state->simple_insert_rowset_cache.size(),
                                 row_count,
                                 cache_hit ? "hit" : "miss_or_stale");
  if (!cache_hit) {
    CopyImportState prepare_template;
    prepare_template.native_bulk_ingest = true;
    prepare_template.native_bulk_ingest_enabled = true;
    prepare_template.sql = rowset.sql;
    prepare_template.target_object_uuid = rowset.target_object_uuid;
    const std::int64_t prepare_started = phase_trace ? ParserPhaseNowNs() : 0;
    const std::string prepare_envelope =
        BuildNativeBulkIngestExecuteEnvelope(prepare_template, 0, 0);
    const auto prepared =
        session->PrepareSblrForWire(prepare_envelope);
    WriteParserPhaseTraceIfEnabled(phase_trace,
                                   "simple_insert_rowset_fast_path",
                                   "prepare_sblr",
                                   prepare_started,
                                   prepare_envelope.size(),
                                   1,
                                   0,
                                   prepared.accepted ? "accepted" : "rejected");
    if (prepared.accepted && !prepared.prepared_statement_uuid.empty()) {
      SimpleInsertRowsetPreparedEntry entry;
      entry.prepared_statement_uuid = prepared.prepared_statement_uuid;
      entry.operation_id = prepared.operation_id;
      entry.target_object_uuid = rowset.target_object_uuid;
      CaptureSimpleInsertRowsetAuthorityEpochs(*session, &entry);
      cache_it =
          state->simple_insert_rowset_cache.insert_or_assign(presented_shape_key,
                                                             std::move(entry)).first;
    } else {
      cache_it = state->simple_insert_rowset_cache.end();
    }
  }

  const std::int64_t envelope_started = phase_trace ? ParserPhaseNowNs() : 0;
  const std::string envelope =
      BuildNativeBulkIngestExecuteEnvelope(rowset, 0, row_count, false);
  WriteParserPhaseTraceIfEnabled(phase_trace,
                                 "simple_insert_rowset_fast_path",
                                 "build_sblr_envelope",
                                 envelope_started,
                                 envelope.size(),
                                 1,
                                 row_count);
  const std::int64_t packet_started = phase_trace ? ParserPhaseNowNs() : 0;
  const std::vector<std::uint8_t> data_packet =
      BuildNativeRowPacket(rowset, 0, row_count);
  WriteParserPhaseTraceIfEnabled(phase_trace,
                                 "simple_insert_rowset_fast_path",
                                 "build_native_row_packet",
                                 packet_started,
                                 data_packet.size(),
                                 1,
                                 row_count,
                                 "scratchbird.native_rows.v2");
  if (data_packet.empty()) {
    write_total_trace("not_applicable_empty_data_packet");
    return std::nullopt;
  }

  const bool uses_prepared = cache_it != state->simple_insert_rowset_cache.end();
  const std::int64_t execute_started = phase_trace ? ParserPhaseNowNs() : 0;
  auto result = cache_it == state->simple_insert_rowset_cache.end()
                    ? session->RunSblrEnvelopeWithDataPacket(envelope, data_packet, false)
                    : session->RunPreparedSblrEnvelopeForWire(
                          cache_it->second.prepared_statement_uuid,
                          envelope,
                          data_packet,
                          false);
  WriteParserPhaseTraceIfEnabled(phase_trace,
                                 "simple_insert_rowset_fast_path",
                                 uses_prepared ? "run_prepared_sblr_with_data_packet"
                                               : "run_sblr_with_data_packet",
                                 execute_started,
                                 envelope.size() + data_packet.size(),
                                 1,
                                 row_count,
                                 result.accepted ? "accepted" : "rejected");
  if (!result.accepted || result.messages.has_errors()) {
    if (command_accepted != nullptr) *command_accepted = false;
    const std::string diagnostic_code =
        FirstDiagnosticCode(result.messages, "SBSQL.INSERT_ROWSET_FAST_PATH.REJECTED");
    const std::string diagnostic_detail = DiagnosticFieldValue(result.messages, "detail");
    if (diagnostic_code.find("PREPARED_STATEMENT") != std::string::npos ||
        diagnostic_detail.find("prepared_statement") != std::string::npos) {
      state->simple_insert_rowset_cache.erase(presented_shape_key);
    }
    if (!SendError(io,
                   state,
                   "42000",
                   FirstDiagnosticText(result.messages),
                   diagnostic_detail.empty() ? diagnostic_code
                                             : diagnostic_code + ";" + diagnostic_detail)) {
      write_total_trace("send_error_failed");
      return false;
    }
    write_total_trace("rejected");
    return !send_ready || SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  RefreshWireTransactionStateFromSession(*session, state);
  if (result.server_row_count == 0) result.server_row_count = row_count;
  result.server_affected_rows_present = true;
  if (result.server_affected_rows == 0) result.server_affected_rows = row_count;
  const std::int64_t send_started = phase_trace ? ParserPhaseNowNs() : 0;
  if (!SendPipelineResult(io, session, state, rowset.sql, result)) {
    WriteParserPhaseTraceIfEnabled(phase_trace,
                                   "simple_insert_rowset_fast_path",
                                   "send_pipeline_result",
                                   send_started,
                                   0,
                                   1,
                                   row_count,
                                   "failed");
    write_total_trace("send_pipeline_result_failed");
    return false;
  }
  WriteParserPhaseTraceIfEnabled(phase_trace,
                                 "simple_insert_rowset_fast_path",
                                 "send_pipeline_result",
                                 send_started,
                                 0,
                                 1,
                                 row_count,
                                 "sent");
  write_total_trace(uses_prepared ? "prepared_success" : "direct_success");
  return !send_ready || SendReady(io, state);
}

std::optional<bool> TryExecuteSimpleInsertRowsetFastPath(SbsqlTestWireSession* session,
                                                         ClientIo* io,
                                                         SbwpSessionState* state,
                                                         std::string_view raw_sql,
                                                         bool send_ready,
                                                         bool autocommit_emulation,
                                                         bool* command_accepted = nullptr) {
  if (command_accepted != nullptr) *command_accepted = true;
  if (autocommit_emulation) return std::nullopt;
  auto rowset = AnalyzeSimpleLiteralInsertRowset(raw_sql);
  if (!rowset.has_value()) return std::nullopt;
  return TryExecuteSimpleInsertRowsetFastPath(session,
                                             io,
                                             state,
                                             std::move(*rowset),
                                             send_ready,
                                             command_accepted);
}

std::string SqlQuote(std::string_view value) {
  std::string out = "'";
  for (char ch : value) {
    if (ch == '\'') out.push_back('\'');
    out.push_back(ch);
  }
  out.push_back('\'');
  return out;
}

std::string StripLengthPrefixText(const std::vector<std::uint8_t>& data) {
  if (data.size() >= 4) {
    const std::uint32_t length = ReadU32(data, 0);
    if (length <= data.size() - 4) {
      return std::string(reinterpret_cast<const char*>(data.data() + 4), length);
    }
  }
  return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

std::string UuidLiteralFromBytes(const std::vector<std::uint8_t>& data) {
  if (data.size() != 16) return "NULL";
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(36);
  for (std::size_t i = 0; i < data.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) text.push_back('-');
    text.push_back(kHex[(data[i] >> 4u) & 0x0fu]);
    text.push_back(kHex[data[i] & 0x0fu]);
  }
  return SqlQuote(text);
}

std::string DecodeParamLiteral(std::uint32_t oid,
                               std::uint16_t format,
                               const std::optional<std::vector<std::uint8_t>>& data) {
  if (!data.has_value()) return "NULL";
  if (format == 0) {
    const std::string text(reinterpret_cast<const char*>(data->data()), data->size());
    if (oid == kOidInt2 || oid == kOidInt4 || oid == kOidInt8 ||
        oid == kOidFloat4 || oid == kOidFloat8 || oid == kOidNumeric ||
        oid == kOidBool) {
      return text;
    }
    return SqlQuote(text);
  }
  if (oid == kOidBool && !data->empty()) return (*data)[0] == 0 ? "FALSE" : "TRUE";
  if (oid == kOidInt2 && data->size() >= 2) {
    return std::to_string(static_cast<std::int16_t>(ReadU16(*data, 0)));
  }
  if (oid == kOidInt4 && data->size() >= 4) {
    return std::to_string(static_cast<std::int32_t>(ReadU32(*data, 0)));
  }
  if (oid == kOidInt8 && data->size() >= 8) {
    return std::to_string(static_cast<std::int64_t>(ReadU64(*data, 0)));
  }
  if (oid == kOidFloat4 && data->size() >= 4) {
    float value = 0.0f;
    std::memcpy(&value, data->data(), sizeof(value));
    std::ostringstream out;
    out << value;
    return out.str();
  }
  if (oid == kOidFloat8 && data->size() >= 8) {
    double value = 0.0;
    std::memcpy(&value, data->data(), sizeof(value));
    std::ostringstream out;
    out << value;
    return out.str();
  }
  if (oid == kOidUuid && data->size() == 16) return UuidLiteralFromBytes(*data);
  const auto text = StripLengthPrefixText(*data);
  if (oid == kOidNumeric) return text;
  if (oid == kOidText || oid == kOidVarchar ||
      oid == kOidDate || oid == kOidTime ||
      oid == kOidTimestamp || oid == kOidTimestamptz) {
    return SqlQuote(text);
  }
  if (oid == 0 && (LooksInteger(text) || LooksDecimal(text))) return text;
  return SqlQuote(text);
}

std::optional<std::string> DecodeParamValue(std::uint32_t oid,
                                            std::uint16_t format,
                                            const std::optional<std::vector<std::uint8_t>>& data) {
  if (!data.has_value()) return std::nullopt;
  if (format == 0) {
    return std::string(reinterpret_cast<const char*>(data->data()), data->size());
  }
  if (oid == kOidBool && !data->empty()) return (*data)[0] == 0 ? "FALSE" : "TRUE";
  if (oid == kOidInt2 && data->size() >= 2) {
    return std::to_string(static_cast<std::int16_t>(ReadU16(*data, 0)));
  }
  if (oid == kOidInt4 && data->size() >= 4) {
    return std::to_string(static_cast<std::int32_t>(ReadU32(*data, 0)));
  }
  if (oid == kOidInt8 && data->size() >= 8) {
    return std::to_string(static_cast<std::int64_t>(ReadU64(*data, 0)));
  }
  if (oid == kOidFloat4 && data->size() >= 4) {
    float value = 0.0f;
    std::memcpy(&value, data->data(), sizeof(value));
    std::ostringstream out;
    out << value;
    return out.str();
  }
  if (oid == kOidFloat8 && data->size() >= 8) {
    double value = 0.0;
    std::memcpy(&value, data->data(), sizeof(value));
    std::ostringstream out;
    out << value;
    return out.str();
  }
  if (oid == kOidUuid && data->size() == 16) {
    std::string literal = UuidLiteralFromBytes(*data);
    if (literal.size() >= 2 && literal.front() == '\'' && literal.back() == '\'') {
      literal = literal.substr(1, literal.size() - 2);
    }
    return literal;
  }
  return StripLengthPrefixText(*data);
}

std::string SubstituteParams(std::string sql, const std::vector<std::string>& literals) {
  std::string out;
  out.reserve(sql.size() + literals.size() * 8);
  bool in_single = false;
  bool in_double = false;
  bool in_line_comment = false;
  bool in_block_comment = false;
  std::size_t question_index = 0;
  for (std::size_t i = 0; i < sql.size();) {
    const char ch = sql[i];
    const char next = i + 1 < sql.size() ? sql[i + 1] : '\0';
    if (in_line_comment) {
      out.push_back(ch);
      in_line_comment = ch != '\n';
      ++i;
      continue;
    }
    if (in_block_comment) {
      out.push_back(ch);
      if (ch == '*' && next == '/') {
        out.push_back(next);
        in_block_comment = false;
        i += 2;
      } else {
        ++i;
      }
      continue;
    }
    if (!in_single && !in_double && ch == '-' && next == '-') {
      out.push_back(ch);
      out.push_back(next);
      in_line_comment = true;
      i += 2;
      continue;
    }
    if (!in_single && !in_double && ch == '/' && next == '*') {
      out.push_back(ch);
      out.push_back(next);
      in_block_comment = true;
      i += 2;
      continue;
    }
    if (ch == '\'' && !in_double) {
      if (in_single && next == '\'') {
        out.push_back(ch);
        out.push_back(next);
        i += 2;
        continue;
      }
      in_single = !in_single;
      out.push_back(ch);
      ++i;
      continue;
    }
    if (ch == '"' && !in_single) {
      if (in_double && next == '"') {
        out.push_back(ch);
        out.push_back(next);
        i += 2;
        continue;
      }
      in_double = !in_double;
      out.push_back(ch);
      ++i;
      continue;
    }
    if (!in_single && !in_double && ch == '$' && i + 1 < sql.size() &&
        std::isdigit(static_cast<unsigned char>(sql[i + 1]))) {
      std::size_t j = i + 1;
      std::uint64_t index = 0;
      while (j < sql.size() && std::isdigit(static_cast<unsigned char>(sql[j]))) {
        index = index * 10u + static_cast<std::uint64_t>(sql[j] - '0');
        ++j;
      }
      if (index > 0 && index <= literals.size()) {
        out += literals[static_cast<std::size_t>(index - 1)];
        i = j;
        continue;
      }
    }
    if (!in_single && !in_double && ch == '?') {
      if (question_index < literals.size()) {
        out += literals[question_index++];
      } else {
        out.push_back(ch);
      }
      ++i;
      continue;
    }
    out.push_back(ch);
    ++i;
  }
  return out;
}

std::optional<BoundPortal> ParseBindPayload(const std::vector<std::uint8_t>& payload,
                                            const std::map<std::string, PreparedStatement>& statements,
                                            std::string* portal_name) {
  std::size_t off = 0;
  const std::string parsed_portal_name = ReadSizedString(payload, &off);
  if (portal_name != nullptr) *portal_name = parsed_portal_name;
  const std::string statement_name = ReadSizedString(payload, &off);
  const auto statement_it = statements.find(statement_name);
  if (statement_it == statements.end()) return std::nullopt;
  const PreparedStatement& statement = statement_it->second;
  if (off + 2 > payload.size()) return std::nullopt;
  const std::uint16_t format_count = ReadU16(payload, off);
  off += 2;
  std::vector<std::uint16_t> formats;
  for (std::uint16_t i = 0; i < format_count && off + 2 <= payload.size(); ++i) {
    formats.push_back(ReadU16(payload, off));
    off += 2;
  }
  if (off + 4 > payload.size()) return std::nullopt;
  const std::uint16_t value_count = ReadU16(payload, off);
  off += 4;
  std::vector<std::string> literals;
  literals.reserve(value_count);
  std::vector<std::optional<std::string>> values;
  values.reserve(value_count);
  for (std::uint16_t i = 0; i < value_count && off + 4 <= payload.size(); ++i) {
    const std::int32_t length = ReadI32(payload, off);
    off += 4;
    std::optional<std::vector<std::uint8_t>> data;
    if (length >= 0) {
      const auto bytes = static_cast<std::size_t>(length);
      if (off + bytes > payload.size()) return std::nullopt;
      data = std::vector<std::uint8_t>(payload.begin() + static_cast<std::ptrdiff_t>(off),
                                       payload.begin() + static_cast<std::ptrdiff_t>(off + bytes));
      off += bytes;
    }
    const std::uint16_t format = formats.empty()
                                     ? 1
                                     : formats[std::min<std::size_t>(i, formats.size() - 1)];
    const std::uint32_t oid = i < statement.param_types.size() ? statement.param_types[i] : 0;
    literals.push_back(DecodeParamLiteral(oid, format, data));
    values.push_back(DecodeParamValue(oid, format, data));
  }
  BoundPortal bound;
  bound.sql = SubstituteParams(statement.sql, literals);
  bound.param_types = statement.param_types;
  bound.param_values = std::move(values);
  bound.param_rows.push_back(bound.param_values);
  bound.insert_rowset_plan = statement.insert_rowset_plan;
  return bound;
}

bool LooksLikeP1ParameterDataBind(const std::vector<std::uint8_t>& payload) {
  return payload.size() >= 4 &&
         ReadU16(payload, 0) == datatypes::kNativeWireMetadataLayoutVersion &&
         payload[2] == static_cast<std::uint8_t>(datatypes::WireParameterPacketKind::bind_request);
}

const PreparedStatement* FindArrayBindStatement(
    const datatypes::ParameterDataPacket& packet,
    const std::map<std::string, PreparedStatement>& statements) {
  const auto parameter_count = packet.slot_descriptors.size();
  const PreparedStatement* selected = nullptr;
  for (const auto& [name, statement] : statements) {
    (void)name;
    if (!statement.insert_rowset_plan.has_value()) continue;
    if (statement.param_types.size() != parameter_count) continue;
    if (selected != nullptr) return nullptr;
    selected = &statement;
  }
  return selected;
}

std::optional<std::optional<std::string>> DecodeParameterPacketCell(
    const datatypes::ParameterValueCell& cell,
    std::uint32_t oid,
    std::string* diagnostic_code) {
  if (cell.value_state == datatypes::WireValueState::sql_null) {
    return std::optional<std::string>{};
  }
  if (cell.value_state != datatypes::WireValueState::value_present) {
    if (diagnostic_code != nullptr) *diagnostic_code = "SBWP.ARRAY_BIND.VALUE_STATE_UNSUPPORTED";
    return std::nullopt;
  }
  const std::optional<std::vector<std::uint8_t>> payload = cell.payload;
  const std::uint16_t format =
      cell.payload_encoding == datatypes::WirePayloadEncoding::utf8_text ? 0 : 1;
  return DecodeParamValue(oid, format, payload);
}

std::optional<BoundPortal> ParseArrayBindPayload(
    const std::vector<std::uint8_t>& payload,
    const std::map<std::string, PreparedStatement>& statements,
    std::string* diagnostic_code) {
  auto decoded = datatypes::DecodeParameterDataPacket(payload);
  if (!decoded.ok) {
    if (diagnostic_code != nullptr) {
      *diagnostic_code = decoded.diagnostic_code.empty()
                             ? "SBWP.ARRAY_BIND.PARAMETER_PACKET_INVALID"
                             : decoded.diagnostic_code;
    }
    return std::nullopt;
  }
  if (decoded.packet.bind_shape != datatypes::WireParameterBindShape::array_bind_rows) {
    if (diagnostic_code != nullptr) *diagnostic_code = "SBWP.ARRAY_BIND.SHAPE_UNSUPPORTED";
    return std::nullopt;
  }
  const PreparedStatement* statement = FindArrayBindStatement(decoded.packet, statements);
  if (statement == nullptr) {
    if (diagnostic_code != nullptr) *diagnostic_code = "SBWP.ARRAY_BIND.STATEMENT_AMBIGUOUS";
    return std::nullopt;
  }
  BoundPortal bound;
  bound.sql = statement->sql;
  bound.param_types = statement->param_types;
  bound.insert_rowset_plan = statement->insert_rowset_plan;
  bound.param_rows.reserve(decoded.packet.row_value_frames.size());
  for (const auto& frame : decoded.packet.row_value_frames) {
    std::vector<std::optional<std::string>> values(statement->param_types.size());
    for (const auto& cell : frame.values) {
      if (cell.slot_ordinal == 0 || cell.slot_ordinal > statement->param_types.size()) {
        if (diagnostic_code != nullptr) *diagnostic_code = "SBWP.ARRAY_BIND.PARAMETER_ORDINAL_INVALID";
        return std::nullopt;
      }
      const std::size_t index = static_cast<std::size_t>(cell.slot_ordinal - 1);
      auto decoded_value = DecodeParameterPacketCell(cell, statement->param_types[index],
                                                     diagnostic_code);
      if (!decoded_value.has_value()) return std::nullopt;
      values[index] = std::move(*decoded_value);
    }
    bound.param_rows.push_back(std::move(values));
  }
  if (!bound.param_rows.empty()) {
    bound.param_values = bound.param_rows.front();
  }
  return bound;
}

std::string ParsePortalName(const std::vector<std::uint8_t>& payload) {
  std::size_t off = 0;
  return ReadSizedString(payload, &off);
}

std::uint32_t ParseExecuteFlags(const std::vector<std::uint8_t>& payload) {
  std::size_t off = 0;
  (void)ReadSizedString(payload, &off);
  if (off + 4 > payload.size()) return 0;
  off += 4;  // max_rows
  if (off + 4 > payload.size()) return 0;
  return ReadU32(payload, off);
}

std::optional<bool> ExecutePreparedInsertRowset(SbsqlTestWireSession* session,
                                                ClientIo* io,
                                                SbwpSessionState* state,
                                                const BoundPortal& portal,
                                                bool send_ready) {
  state->ready_sent_for_current_operation = false;
  if (!portal.insert_rowset_plan.has_value()) return std::nullopt;
  auto plan = *portal.insert_rowset_plan;
  if (plan.target_object_uuid.empty()) {
    auto resolved = session->ResolvePublicNameForWire(plan.target_name, false, "relation");
    if (!resolved.resolved || resolved.object_uuid.empty()) {
      if (!SendError(io,
                     state,
                     "42000",
                     "SBSQL.NAME_RESOLUTION.NOT_FOUND_OR_NOT_VISIBLE",
                     "prepared rowset target object could not be resolved")) {
        return false;
      }
      return !send_ready || SendReady(io, state, ReadyReason::kErrorRecovered);
    }
    plan.target_object_uuid = std::move(resolved.object_uuid);
  }
  CopyImportState rowset;
  rowset.native_bulk_ingest = true;
  rowset.native_bulk_ingest_enabled = true;
  rowset.sql = portal.sql;
  rowset.target_object_uuid = plan.target_object_uuid;
  auto append_parameter_row = [&](const std::vector<std::optional<std::string>>& values) -> bool {
    CopyImportRow row;
    row.fields.reserve(plan.column_names.size());
    for (std::size_t i = 0; i < plan.column_names.size(); ++i) {
      const std::size_t parameter_index =
          i < plan.parameter_indexes.size() ? plan.parameter_indexes[i] : values.size();
      if (parameter_index >= values.size()) {
        if (!SendError(io,
                       state,
                       "08P01",
                       "SBWP.BIND.PARAMETER_MISSING",
                       "prepared rowset bind did not supply every required parameter")) {
          return false;
        }
        return !send_ready || SendReady(io, state, ReadyReason::kErrorRecovered);
      }
      row.fields.emplace_back(plan.column_names[i], values[parameter_index]);
    }
    rowset.rows.push_back(std::move(row));
    return true;
  };
  rowset.rows.reserve(portal.param_rows.empty() ? 1 : portal.param_rows.size());
  if (portal.param_rows.empty()) {
    if (!append_parameter_row(portal.param_values)) return false;
  } else {
    for (const auto& values : portal.param_rows) {
      if (!append_parameter_row(values)) return false;
    }
  }
  const std::string envelope = BuildNativeBulkIngestExecuteEnvelope(rowset, 0, rowset.rows.size());
  auto result = plan.server_prepared_statement_uuid.empty()
                    ? session->RunSblrEnvelope(envelope, false)
                    : session->RunPreparedSblrEnvelopeForWire(
                          plan.server_prepared_statement_uuid, envelope, {}, false);
  if (!result.accepted || result.messages.has_errors()) {
    const std::string diagnostic_code =
        FirstDiagnosticCode(result.messages, "SBWP.PREPARED_ROWSET.EXECUTION_REJECTED");
    const std::string diagnostic_detail = DiagnosticFieldValue(result.messages, "detail");
    if (!SendError(io,
                   state,
                   "42000",
                   FirstDiagnosticText(result.messages),
                   diagnostic_detail.empty() ? diagnostic_code
                                             : diagnostic_code + ";" + diagnostic_detail)) {
      return false;
    }
    return !send_ready || SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  RefreshWireTransactionStateFromSession(*session, state);
  if (result.server_row_count == 0) result.server_row_count = rowset.rows.size();
  if (!SendPipelineResult(io, session, state, portal.sql, result)) {
    return false;
  }
  return !send_ready || SendReady(io, state);
}

bool ExecuteNativeSblrFrame(SbsqlTestWireSession* session,
                            ClientIo* io,
                            SbwpSessionState* state,
                            const Frame& frame) {
  state->ready_sent_for_current_operation = false;
  if (frame.payload.size() < 16) {
    return SendError(io,
                     state,
                     "08P01",
                     "SBWP.SBLR_EXECUTE.PAYLOAD_INVALID",
                     "native SBLR execute payload is truncated") &&
           SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  std::size_t off = 0;
  const std::uint64_t sblr_hash = ReadU64(frame.payload, off);
  (void)sblr_hash;
  off += 8;
  const std::uint32_t bytecode_size = ReadU32(frame.payload, off);
  off += 4;
  const std::uint16_t parameter_count = ReadU16(frame.payload, off);
  off += 4;
  if (off + bytecode_size > frame.payload.size()) {
    return SendError(io,
                     state,
                     "08P01",
                     "SBWP.SBLR_EXECUTE.PAYLOAD_INVALID",
                     "native SBLR execute bytecode length exceeds payload length") &&
           SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  if (parameter_count != 0) {
    return SendError(io,
                     state,
                     "0A000",
                     "SBWP.SBLR_EXECUTE.PARAMETER_BINDING_REQUIRED",
                     "native SBLR execute parameters must use an admitted rowset binding route") &&
           SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  const std::string encoded_sblr_envelope(
      reinterpret_cast<const char*>(frame.payload.data() + off), bytecode_size);
  if (encoded_sblr_envelope.empty()) {
    return SendError(io,
                     state,
                     "08P01",
                     "SBWP.SBLR_EXECUTE.EMPTY_ENVELOPE",
                     "native SBLR execute requires a non-empty admitted SBLR envelope") &&
           SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  auto result = session->RunSblrEnvelope(encoded_sblr_envelope, false);
  if (!result.accepted || result.messages.has_errors()) {
    return SendError(io,
                     state,
                     "42000",
                     FirstDiagnosticText(result.messages),
                     FirstDiagnosticCode(result.messages,
                                         "SBWP.SBLR_EXECUTE.REJECTED")) &&
           SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  RefreshWireTransactionStateFromSession(*session, state);
  if (!SendPipelineResult(io, session, state, "/* native sblr */", result)) return false;
  return SendReady(io, state);
}

bool ExecuteSql(SbsqlTestWireSession* session,
                ClientIo* io,
                SbwpSessionState* state,
                std::string_view raw_sql,
                bool send_ready,
                bool autocommit_emulation = false,
                const SbwpTxnCommitRequest* commit_request = nullptr,
                bool* command_accepted = nullptr) {
  const bool phase_trace = ParserPhaseTraceEnabled();
  const std::int64_t total_started = phase_trace ? ParserPhaseNowNs() : 0;
  state->ready_sent_for_current_operation = false;
  if (command_accepted != nullptr) *command_accepted = true;
  const std::string sql = StripSqlTerminator(std::string(raw_sql));
  if (sql.empty()) {
    if (!SendFrame(io, state, kCommandComplete, CommandCompletePayload(0, "EMPTY"))) return false;
    WriteParserPhaseTraceIfEnabled(phase_trace,
                                   "execute_sql",
                                   "total",
                                   total_started,
                                   raw_sql.size(),
                                   1,
                                   0,
                                   "empty");
    return !send_ready || SendReady(io, state);
  }
  if (auto fast_insert =
          TryExecuteSimpleInsertRowsetFastPath(
              session, io, state, sql, send_ready, autocommit_emulation, command_accepted);
      fast_insert.has_value()) {
    WriteParserPhaseTraceIfEnabled(phase_trace,
                                   "execute_sql",
                                   "total",
                                   total_started,
                                   sql.size(),
                                   1,
                                   0,
                                   *fast_insert ? "fast_insert_handled" : "fast_insert_failed");
    return *fast_insert;
  }
  const std::uint64_t original_txn_id = state->txn_id;
  const bool auto_cursor = ShouldAutoCursor(sql);
  const std::int64_t pipeline_started = phase_trace ? ParserPhaseNowNs() : 0;
  const auto result = session->RunPipeline(sql,
                                           true,
                                           auto_cursor,
                                           0,
                                           autocommit_emulation && !auto_cursor);
  WriteParserPhaseTraceIfEnabled(phase_trace,
                                 "execute_sql",
                                 "run_pipeline",
                                 pipeline_started,
                                 sql.size(),
                                 1,
                                 0,
                                 result.accepted ? "accepted" : "rejected");
  if (!result.accepted || result.messages.has_errors()) {
    if (command_accepted != nullptr) *command_accepted = false;
    if (commit_request != nullptr && IsPostInventorySecondaryFailure(result.messages)) {
      SbwpTxnFinalityRecord record;
      record.state = SbwpTxnFinalityState::kPostInventorySecondaryFailure;
      record.flags = kTxnFinalityFlagEngineKnown |
                     kTxnFinalityFlagSameIdempotencyKeyReplayable |
                     kTxnFinalityFlagPostInventorySecondaryFailure;
      record.idempotency_key = commit_request->idempotency_key;
      record.finality_token =
          GeneratedFinalityToken(original_txn_id, state->server_sequence, record.idempotency_key);
      record.request_fingerprint = commit_request->request_fingerprint;
      record.original_txn_id = original_txn_id;
      record.replacement_txn_id = TransactionIdFromResultPayload(result.server_result_payload)
                                      .value_or(original_txn_id);
      record.diagnostic_code = "SBWP.COMMIT.POST_INVENTORY_SECONDARY_FAILURE";
      record.detail =
          "commit is final in MGA transaction inventory; secondary post-inventory work failed";
      StoreFinalityRecord(state, record);
      if (!SendTxnFinalityStatus(io, state, record)) return false;
    }
    if (!SendError(io,
                   state,
                   "42000",
                   FirstDiagnosticText(result.messages),
                   FirstDiagnosticCode(result.messages, "SBSQL.EXECUTION.REJECTED"))) {
      return false;
    }
    WriteParserPhaseTraceIfEnabled(phase_trace,
                                   "execute_sql",
                                   "total",
                                   total_started,
                                   sql.size(),
                                   1,
                                   0,
                                   "rejected");
    return !send_ready || SendReady(io, state);
  }
  RefreshWireTransactionStateFromSession(*session, state);
  if (SbwpOperationInvalidatesSimpleInsertPreparedCache(result.server_operation_id)) {
    state->simple_insert_rowset_cache.clear();
  }
  if (result.server_operation_id == "transaction.begin") {
    if (const auto local_id = TransactionIdFromResultPayload(result.server_result_payload)) {
      state->txn_id = *local_id;
    }
  } else if (result.server_operation_id == "transaction.commit" ||
             result.server_operation_id == "transaction.rollback") {
    const auto replacement = TransactionIdFromResultPayload(result.server_result_payload);
    if (!replacement.has_value() || *replacement == 0) {
      if (!SendError(io,
                     state,
                     "25000",
                     "MGA.TRANSACTION.REPLACEMENT_MISSING",
                     "commit or rollback did not publish the required replacement transaction")) {
        return false;
      }
      return !send_ready || SendReady(io, state, ReadyReason::kErrorRecovered);
    }
    state->txn_id = *replacement;
  }
  std::optional<SbwpTxnFinalityRecord> commit_finality;
  if (commit_request != nullptr &&
      (commit_request->contract_flags & kTxnCommitFlagHasIdempotencyKey) != 0 &&
      !IsZeroUuid(commit_request->idempotency_key) &&
      result.server_operation_id == "transaction.commit") {
    SbwpTxnFinalityRecord record;
    record.state = SbwpTxnFinalityState::kCommitted;
    record.flags = kTxnFinalityFlagEngineKnown | kTxnFinalityFlagSameIdempotencyKeyReplayable;
    record.idempotency_key = commit_request->idempotency_key;
    record.finality_token =
        GeneratedFinalityToken(original_txn_id, state->server_sequence, record.idempotency_key);
    record.request_fingerprint = commit_request->request_fingerprint;
    record.original_txn_id = original_txn_id;
    record.replacement_txn_id = state->txn_id;
    record.diagnostic_code = "SBWP.COMMIT.FINALITY_COMMITTED_BY_MGA_INVENTORY";
    record.detail =
        "committed_by_engine_mga_inventory;retry_side_effects_require_caller_acknowledgement";
    StoreFinalityRecord(state, record);
    commit_finality = std::move(record);
  }
  if (result.server_operation_id == "dml.plan_import_rows" &&
      Upper(sql).find("FROM STDIN") != std::string::npos) {
    const auto target_uuid = JsonObjectTextField(result.sblr_payload, "target_object_uuid");
    if (!target_uuid.has_value() || target_uuid->empty()) {
      if (!SendError(io,
                     state,
                     "42000",
                     "SBSQL.COPY.TARGET_UUID_MISSING",
                     "COPY FROM STDIN requires a UUID-bound target before CopyData")) {
        return false;
      }
      return !send_ready || SendReady(io, state);
    }
    state->copy_import = CopyImportState{};
    state->copy_import.active = true;
    const bool native_bulk_ingest_requested = NativeBulkIngestRequested(sql);
    const bool native_bulk_ingest_enabled = NativeBulkIngestEnabledRequested(sql);
    state->copy_import.native_bulk_ingest_enabled = native_bulk_ingest_enabled;
    state->copy_import.native_bulk_ingest =
        native_bulk_ingest_enabled || native_bulk_ingest_requested;
    state->copy_import.sql = sql;
    state->copy_import.target_object_uuid = *target_uuid;
    const auto copy_in_response = CopyInResponsePayload(state);
  std::string prepare_code;
  std::string prepare_detail;
  if (!PrepareCopyNativeBulkHandle(session,
                                     state,
                                     &state->copy_import,
                                     &prepare_code,
                                     &prepare_detail)) {
      if (prepare_code != "SBSQL.PREPARE.UNAVAILABLE") {
        state->copy_import = CopyImportState{};
        if (!SendError(io,
                       state,
                       "42000",
                       prepare_code.empty() ? "SBSQL.COPY.PREPARED_HANDLE_REFUSED"
                                            : prepare_code,
                       prepare_detail.empty()
                           ? "COPY native bulk path requires a server-owned prepared SBLR handle"
                           : prepare_detail)) {
          return false;
        }
        return !send_ready || SendReady(io, state, ReadyReason::kErrorRecovered);
      }
    }
    if (!SendFrame(io, state, kCopyInResponse, copy_in_response)) return false;
    return true;
  }
  if (!SendPipelineResult(io, session, state, sql, result)) return false;
  if (commit_finality.has_value() && !SendTxnFinalityStatus(io, state, *commit_finality)) {
    return false;
  }
  WriteParserPhaseTraceIfEnabled(phase_trace,
                                 "execute_sql",
                                 "total",
                                 total_started,
                                 sql.size(),
                                 1,
                                 0,
                                 result.server_operation_id);
  return !send_ready || SendReady(io, state);
}

bool QueryPayloadRequestsScriptIngest(const QueryPayload& query) {
  return (query.flags & kQueryFlagScriptIngest) != 0 ||
         (query.flags & kQueryFlagScriptSizeHint) != 0 ||
         query.has_script_metadata ||
         query.expected_statement_count > 1;
}

bool ExecuteScriptOrSql(SbsqlTestWireSession* session,
                        ClientIo* io,
                        SbwpSessionState* state,
                        const QueryPayload& query,
                        bool send_ready) {
  const bool phase_trace = ParserPhaseTraceEnabled();
  const std::int64_t total_started = phase_trace ? ParserPhaseNowNs() : 0;
  const bool autocommit_emulation = (query.flags & kQueryFlagAutocommit) != 0;
  std::vector<std::string> statements;
  if (QueryPayloadRequestsScriptIngest(query) || query.sql.find(';') != std::string::npos) {
    const std::int64_t split_started = phase_trace ? ParserPhaseNowNs() : 0;
    statements = SplitSbwpScriptStatements(query.sql);
    WriteParserPhaseTraceIfEnabled(phase_trace,
                                   "execute_script_or_sql",
                                   "split_script_statements",
                                   split_started,
                                   query.sql.size(),
                                   statements.size(),
                                   0,
                                   QueryPayloadRequestsScriptIngest(query) ? "script_ingest"
                                                                           : "semicolon_detected");
  }
  if (statements.size() <= 1) {
    const std::string_view sql =
        statements.empty() ? std::string_view(query.sql)
                           : std::string_view(statements.front());
    const bool ok = ExecuteSql(session, io, state, sql, send_ready, autocommit_emulation);
    WriteParserPhaseTraceIfEnabled(phase_trace,
                                   "execute_script_or_sql",
                                   "total",
                                   total_started,
                                   query.sql.size(),
                                   statements.empty() ? 1 : statements.size(),
                                   0,
                                   ok ? "single_statement" : "single_statement_failed");
    return ok;
  }

  struct PendingInsertGroup {
    CopyImportState rowset;
    std::vector<std::string> fallback_statements;
    bool active{false};
  } pending;

  const auto flush_pending = [&]() -> bool {
    if (!pending.active) return true;
    bool accepted = true;
    const std::size_t pending_rows = pending.rowset.rows.size();
    const std::size_t fallback_count = pending.fallback_statements.size();
    const std::int64_t fast_started = phase_trace ? ParserPhaseNowNs() : 0;
    auto fast_result = TryExecuteSimpleInsertRowsetFastPath(session,
                                                           io,
                                                           state,
                                                           std::move(pending.rowset),
                                                           false,
                                                           &accepted);
    WriteParserPhaseTraceIfEnabled(phase_trace,
                                   "script_insert_group",
                                   "flush_fast_path",
                                   fast_started,
                                   0,
                                   fallback_count,
                                   pending_rows,
                                   fast_result.has_value()
                                       ? (accepted ? "handled" : "rejected")
                                       : "not_applicable");
    if (fast_result.has_value()) {
      pending = PendingInsertGroup{};
      if (!*fast_result) return false;
      if (!accepted) {
        return !send_ready || SendReady(io, state, ReadyReason::kErrorRecovered);
      }
      return true;
    }
    const auto fallback = std::move(pending.fallback_statements);
    pending = PendingInsertGroup{};
    const std::int64_t fallback_started = phase_trace ? ParserPhaseNowNs() : 0;
    for (const auto& sql : fallback) {
      accepted = true;
      if (!ExecuteSql(session, io, state, sql, false, autocommit_emulation, nullptr, &accepted)) {
        WriteParserPhaseTraceIfEnabled(phase_trace,
                                       "script_insert_group",
                                       "fallback_statement_execution",
                                       fallback_started,
                                       0,
                                       fallback.size(),
                                       pending_rows,
                                       "failed");
        return false;
      }
      if (!accepted) {
        WriteParserPhaseTraceIfEnabled(phase_trace,
                                       "script_insert_group",
                                       "fallback_statement_execution",
                                       fallback_started,
                                       0,
                                       fallback.size(),
                                       pending_rows,
                                       "rejected");
        return !send_ready || SendReady(io, state, ReadyReason::kErrorRecovered);
      }
    }
    WriteParserPhaseTraceIfEnabled(phase_trace,
                                   "script_insert_group",
                                   "fallback_statement_execution",
                                   fallback_started,
                                   0,
                                   fallback.size(),
                                   pending_rows,
                                   "accepted");
    return true;
  };

  for (const auto& statement : statements) {
    if (!autocommit_emulation) {
      auto rowset = AnalyzeSimpleLiteralInsertRowset(statement, true);
      if (rowset.has_value()) {
        if (!pending.active) {
          pending.rowset = std::move(*rowset);
          pending.fallback_statements.push_back(statement);
          pending.active = true;
          if (pending.rowset.rows.size() >= kScriptInsertGroupFlushRows &&
              !flush_pending()) {
            return false;
          }
          continue;
        }
        if (CompatibleSimpleInsertRowsetsForScript(pending.rowset, *rowset)) {
          AppendSimpleInsertRowsForScript(&pending.rowset, *rowset);
          pending.fallback_statements.push_back(statement);
          if (pending.rowset.rows.size() >= kScriptInsertGroupFlushRows &&
              !flush_pending()) {
            return false;
          }
          continue;
        }
        if (!flush_pending()) return false;
        pending.rowset = std::move(*rowset);
        pending.fallback_statements.push_back(statement);
        pending.active = true;
        if (pending.rowset.rows.size() >= kScriptInsertGroupFlushRows &&
            !flush_pending()) {
          return false;
        }
        continue;
      }
    }

    if (!flush_pending()) return false;
    bool accepted = true;
    if (!ExecuteSql(session,
                    io,
                    state,
                    statement,
                    false,
                    autocommit_emulation,
                    nullptr,
                    &accepted)) {
      return false;
    }
    if (!accepted) {
      return !send_ready || SendReady(io, state, ReadyReason::kErrorRecovered);
    }
  }

  if (!flush_pending()) return false;
  WriteParserPhaseTraceIfEnabled(phase_trace,
                                 "execute_script_or_sql",
                                 "total",
                                 total_started,
                                 query.sql.size(),
                                 statements.size(),
                                 0,
                                 "multi_statement");
  return !send_ready || SendReady(io, state);
}

void EnsureNativeCopyAggregate(CopyImportState* copy) {
  if (copy == nullptr || copy->aggregate_result_active) return;
  copy->aggregate_result = PipelineResult{};
  copy->aggregate_result.accepted = true;
  copy->aggregate_result.statement_family = "dml";
  copy->aggregate_result.operation_family = "sblr.dml.operation.v3";
  copy->aggregate_result.server_operation_id = "dml.execute_native_bulk_ingest";
  copy->aggregate_result_active = true;
}

void FoldNativeCopyChunkResult(CopyImportState* copy,
                               const PipelineResult& chunk_result,
                               std::uint64_t fallback_row_count) {
  if (copy == nullptr) return;
  EnsureNativeCopyAggregate(copy);
  auto& aggregate = copy->aggregate_result;
  aggregate.statement_family = chunk_result.statement_family;
  aggregate.operation_family = chunk_result.operation_family;
  aggregate.server_operation_id = chunk_result.server_operation_id;
  aggregate.server_row_count += chunk_result.server_row_count == 0
                                    ? fallback_row_count
                                    : chunk_result.server_row_count;
  aggregate.server_affected_rows +=
      chunk_result.server_affected_rows_present
          ? chunk_result.server_affected_rows
          : (chunk_result.server_row_count == 0 ? fallback_row_count
                                                : chunk_result.server_row_count);
  aggregate.server_affected_rows_present = true;
  if (!chunk_result.server_result_payload.empty()) {
    if (!aggregate.server_result_payload.empty() &&
        aggregate.server_result_payload.back() != '\n') {
      aggregate.server_result_payload.push_back('\n');
    }
    aggregate.server_result_payload += chunk_result.server_result_payload;
  }
  ++copy->aggregate_chunk_count;
}

bool ExecutePreparedNativeCopyPacket(SbsqlTestWireSession* session,
                                     SbwpSessionState* state,
                                     CopyImportState* copy,
                                     const NativeCopyPacket& packet,
                                     bool phase_trace,
                                     std::string* diagnostic_code,
                                     std::string* diagnostic_detail) {
  if (diagnostic_code != nullptr) diagnostic_code->clear();
  if (diagnostic_detail != nullptr) diagnostic_detail->clear();
  if (session == nullptr || state == nullptr || copy == nullptr) {
    if (diagnostic_code != nullptr) {
      *diagnostic_code = "SBSQL.COPY.PREPARED_HANDLE_CONTEXT_MISSING";
    }
    if (diagnostic_detail != nullptr) {
      *diagnostic_detail =
          "COPY native packet execution requires parser session state";
    }
    return false;
  }
  RefreshWireAuthorityEpochsFromSession(*session, state);
  const bool use_prepared = CopyPreparedHandleCurrent(*copy, session->session());
  if (!copy->prepared_statement_uuid.empty() && !use_prepared) {
    if (diagnostic_code != nullptr) {
      *diagnostic_code = "SBSQL.COPY.PREPARED_HANDLE_STALE";
    }
    if (diagnostic_detail != nullptr) {
      *diagnostic_detail =
          "COPY prepared bulk handle is missing or stale for the current session epochs";
    }
    return false;
  }

  const std::size_t chunk_index = copy->aggregate_chunk_count + 1;
  const std::int64_t descriptor_started = phase_trace ? ParserPhaseNowNs() : 0;
  const bool descriptor_already_bound = copy->native_packet_descriptor_bound;
  if (!BindOrValidateNativeCopyDescriptor(copy,
                                          packet,
                                          diagnostic_code,
                                          diagnostic_detail)) {
    WriteParserPhaseTraceIfEnabled(phase_trace,
                                   "copy_data",
                                   "validate_prepared_native_descriptor",
                                   descriptor_started,
                                   packet.payload.size(),
                                   chunk_index,
                                   packet.row_count,
                                   diagnostic_code == nullptr || diagnostic_code->empty()
                                       ? "descriptor_rejected"
                                       : *diagnostic_code);
    return false;
  }
  WriteParserPhaseTraceIfEnabled(phase_trace,
                                 "copy_data",
                                 "validate_prepared_native_descriptor",
                                 descriptor_started,
                                 packet.payload.size(),
                                 chunk_index,
                                 packet.row_count,
                                 descriptor_already_bound ? "descriptor_current"
                                                          : "descriptor_bound");
  const std::int64_t execute_started = phase_trace ? ParserPhaseNowNs() : 0;
  const std::string envelope =
      use_prepared ? std::string{} : BuildNativeBulkIngestExecuteEnvelopeForPacket(*copy, packet);
  auto chunk_result =
      use_prepared
          ? session->RunPreparedSblrEnvelopeForWire(copy->prepared_statement_uuid,
                                                    {},
                                                    packet.payload,
                                                    false)
          : session->RunSblrEnvelopeWithDataPacket(envelope, packet.payload, false);
  WriteParserPhaseTraceIfEnabled(phase_trace,
                                 "copy_data",
                                 use_prepared ? "run_prepared_native_bulk_data_packet"
                                              : "run_native_bulk_data_packet",
                                 execute_started,
                                 envelope.size() + packet.payload.size(),
                                 chunk_index,
                                 packet.row_count,
                                 chunk_result.accepted ? "accepted" : "rejected");
  if (!chunk_result.accepted || chunk_result.messages.has_errors()) {
    if (diagnostic_code != nullptr) {
      *diagnostic_code =
          FirstDiagnosticCode(chunk_result.messages, "SBSQL.COPY.EXECUTION_REJECTED");
    }
    if (diagnostic_detail != nullptr) {
      *diagnostic_detail = DiagnosticFieldValue(chunk_result.messages, "detail");
      if (diagnostic_detail->empty()) {
        *diagnostic_detail = FirstDiagnosticText(chunk_result.messages);
      }
    }
    return false;
  }
  RefreshWireTransactionStateFromSession(*session, state);
  FoldNativeCopyChunkResult(copy, chunk_result, packet.row_count);
  return true;
}

bool HandleCopyData(SbsqlTestWireSession* session,
                    ClientIo* io,
                    SbwpSessionState* state,
                    const Frame& frame) {
  const bool phase_trace = ParserPhaseTraceEnabled();
  const std::int64_t parse_started = phase_trace ? ParserPhaseNowNs() : 0;
  if (!state->authenticated || !state->copy_import.active) {
    return SendError(io,
                     state,
                     "08P01",
                     "ASYNC-011",
                     "COPY_DATA arrived without a prior accepted COPY initiation") &&
           SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  if (state->copy_import.native_bulk_ingest &&
      state->copy_import.copy_data_format == kCopyFormatBinaryRowsetV1 &&
      frame.payload.size() >= 4 &&
      frame.payload[0] == 'S' && frame.payload[1] == 'B' &&
      frame.payload[2] == 'N' && frame.payload[3] == 'R') {
    auto packet = ParseNativeRowCopyPacketHeader(frame.payload);
    WriteParserPhaseTraceIfEnabled(phase_trace,
                                   "copy_data",
                                   "validate_native_packet",
                                   parse_started,
                                   frame.payload.size(),
                                   1,
                                   packet.has_value() ? packet->row_count : 0,
                                   "native_row_packet_passthrough");
    if (!packet.has_value()) {
      state->copy_import = CopyImportState{};
      if (session != nullptr) {
        RefreshWireTransactionStateFromSession(*session, state);
      }
      return SendError(io,
                       state,
                       "22000",
                       "SBSQL.COPY.DATA_ROW_INVALID",
                       "CopyData native row packet is malformed") &&
             SendReady(io, state, ReadyReason::kErrorRecovered);
    }
    std::string diagnostic_code;
    std::string diagnostic_detail;
    if (!ExecutePreparedNativeCopyPacket(session,
                                         state,
                                         &state->copy_import,
                                         *packet,
                                         phase_trace,
                                         &diagnostic_code,
                                         &diagnostic_detail)) {
      state->copy_import = CopyImportState{};
      return SendError(io,
                       state,
                       "42000",
                       diagnostic_code.empty() ? "SBSQL.COPY.EXECUTION_REJECTED"
                                               : diagnostic_code,
                       diagnostic_detail) &&
             SendReady(io, state, ReadyReason::kErrorRecovered);
    }
    return true;
  }
  const auto rows = state->copy_import.copy_data_format == kCopyFormatBinaryRowsetV1
                        ? ParseBinaryCopyRows(frame.payload)
                        : ParseTextCopyRows(frame.payload);
  WriteParserPhaseTraceIfEnabled(phase_trace,
                                 "copy_data",
                                 "parse_payload",
                                 parse_started,
                                 frame.payload.size(),
                                 1,
                                 rows.has_value() ? rows->size() : 0,
                                 state->copy_import.copy_data_format == kCopyFormatBinaryRowsetV1
                                     ? "binary_rowset_v1"
                                     : "canonical_text");
  if (!rows.has_value()) {
    const bool was_binary_copy =
        state->copy_import.copy_data_format == kCopyFormatBinaryRowsetV1;
    state->copy_import = CopyImportState{};
    if (session != nullptr) {
      RefreshWireTransactionStateFromSession(*session, state);
    }
    return SendError(io,
                     state,
                     "22000",
                     "SBSQL.COPY.DATA_ROW_INVALID",
                     was_binary_copy
                         ? "CopyData binary rowset frame is malformed"
                         : "CopyData payload must contain newline-delimited canonical field=value rows") &&
           SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  state->copy_import.rows.reserve(state->copy_import.rows.size() + rows->size());
  state->copy_import.rows.insert(state->copy_import.rows.end(),
                                 std::make_move_iterator(rows->begin()),
                                 std::make_move_iterator(rows->end()));
  return true;
}

bool HandleCopyDone(SbsqlTestWireSession* session, ClientIo* io, SbwpSessionState* state) {
  const bool phase_trace = ParserPhaseTraceEnabled();
  const std::int64_t total_started = phase_trace ? ParserPhaseNowNs() : 0;
  if (!state->authenticated || !state->copy_import.active) {
    return SendError(io,
                     state,
                     "08P01",
                     "ASYNC-011",
                     "COPY_DONE arrived without a prior accepted COPY initiation") &&
           SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  if (state->copy_import.rows.empty() && state->copy_import.native_packets.empty() &&
      !state->copy_import.aggregate_result_active) {
    state->copy_import = CopyImportState{};
    return SendError(io,
                     state,
                     "22000",
                     "SBSQL.COPY.NO_ROWS",
                     "COPY FROM STDIN requires at least one canonical input row before CopyDone") &&
           SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  const std::string copy_sql = state->copy_import.sql;
  CopyImportState copy = std::move(state->copy_import);
  state->copy_import = CopyImportState{};
  PipelineResult result;
  if (copy.aggregate_result_active) {
    result = copy.aggregate_result;
  } else {
    result.accepted = true;
    result.statement_family = "dml";
    result.operation_family = "sblr.dml.operation.v3";
    result.server_operation_id =
        copy.native_bulk_ingest ? "dml.execute_native_bulk_ingest" : "dml.execute_import_rows";
  }
  RefreshWireAuthorityEpochsFromSession(*session, state);
  if (copy.native_bulk_ingest &&
      !copy.prepared_statement_uuid.empty() &&
      !CopyPreparedHandleCurrent(copy, session->session())) {
    return SendError(io,
                     state,
                     "42000",
                     "SBSQL.COPY.PREPARED_HANDLE_STALE",
                     "COPY prepared bulk handle is missing or stale for the current session epochs") &&
           SendReady(io, state, ReadyReason::kErrorRecovered);
  }
  std::size_t chunk_count = copy.aggregate_chunk_count;
  for (const auto& packet : copy.native_packets) {
    std::string diagnostic_code;
    std::string diagnostic_detail;
    if (!BindOrValidateNativeCopyDescriptor(&copy,
                                            packet,
                                            &diagnostic_code,
                                            &diagnostic_detail)) {
      if (!SendError(io,
                     state,
                     "42000",
                     diagnostic_code.empty() ? "SBSQL.COPY.DESCRIPTOR_MISMATCH"
                                             : diagnostic_code,
                     diagnostic_detail)) {
        return false;
      }
      return SendReady(io, state, ReadyReason::kErrorRecovered);
    }
    const std::int64_t execute_started = phase_trace ? ParserPhaseNowNs() : 0;
    const bool use_prepared =
        CopyPreparedHandleCurrent(copy, session->session());
    const std::string envelope =
        use_prepared ? std::string{} : BuildNativeBulkIngestExecuteEnvelopeForPacket(copy, packet);
    auto chunk_result =
        use_prepared
            ? session->RunPreparedSblrEnvelopeForWire(
                  copy.prepared_statement_uuid, {}, packet.payload, false)
            : session->RunSblrEnvelopeWithDataPacket(envelope, packet.payload, false);
    WriteParserPhaseTraceIfEnabled(phase_trace,
                                   "copy_done",
                                   use_prepared ? "run_prepared_sblr_with_data_packet"
                                                : "run_sblr_with_data_packet",
                                   execute_started,
                                   envelope.size() + packet.payload.size(),
                                   chunk_count + 1,
                                   packet.row_count,
                                   chunk_result.accepted ? "accepted" : "rejected");
    if (!chunk_result.accepted || chunk_result.messages.has_errors()) {
      const std::string diagnostic_code = FirstDiagnosticCode(
          chunk_result.messages, "SBSQL.COPY.EXECUTION_REJECTED");
      const std::string diagnostic_detail =
          DiagnosticFieldValue(chunk_result.messages, "detail");
      if (!SendError(io,
                     state,
                     "42000",
                     FirstDiagnosticText(chunk_result.messages),
                     diagnostic_detail.empty() ? diagnostic_code
                                               : diagnostic_code + ";" + diagnostic_detail)) {
        return false;
      }
      return SendReady(io, state, ReadyReason::kErrorRecovered);
    }
    ++chunk_count;
    result.statement_family = chunk_result.statement_family;
    result.operation_family = chunk_result.operation_family;
    result.server_operation_id = chunk_result.server_operation_id;
    result.server_row_count += chunk_result.server_row_count == 0 ? packet.row_count
                                                                  : chunk_result.server_row_count;
    result.server_affected_rows += chunk_result.server_affected_rows_present
                                       ? chunk_result.server_affected_rows
                                       : (chunk_result.server_row_count == 0
                                              ? packet.row_count
                                              : chunk_result.server_row_count);
    result.server_affected_rows_present = true;
    if (!chunk_result.server_result_payload.empty()) {
      if (!result.server_result_payload.empty() && result.server_result_payload.back() != '\n') {
        result.server_result_payload.push_back('\n');
      }
      result.server_result_payload += chunk_result.server_result_payload;
    }
  }
  for (std::size_t first_row = 0; first_row < copy.rows.size();
       first_row += kCopyExecuteRowsPerSblrEnvelope) {
    const std::size_t row_count =
        std::min(kCopyExecuteRowsPerSblrEnvelope, copy.rows.size() - first_row);
    const std::size_t end_row = std::min(copy.rows.size(), first_row + row_count);
    const bool use_native_row_packet =
        copy.native_bulk_ingest && CopyRowsHaveSharedShape(copy, first_row, end_row);
    const std::int64_t envelope_started = phase_trace ? ParserPhaseNowNs() : 0;
    const std::string envelope =
        copy.native_bulk_ingest
            ? BuildNativeBulkIngestExecuteEnvelope(
                  copy, first_row, row_count, !use_native_row_packet)
            : BuildCopyExecuteEnvelope(copy, first_row, row_count);
    WriteParserPhaseTraceIfEnabled(phase_trace,
                                   "copy_done",
                                   "build_execute_envelope",
                                   envelope_started,
                                   envelope.size(),
                                   chunk_count + 1,
                                   row_count,
                                   use_native_row_packet ? "native_row_packet"
                                                         : "inline_rows");
    const std::int64_t packet_started = phase_trace ? ParserPhaseNowNs() : 0;
    const std::vector<std::uint8_t> data_packet =
        use_native_row_packet ? BuildNativeRowPacket(copy, first_row, row_count)
                              : std::vector<std::uint8_t>{};
    WriteParserPhaseTraceIfEnabled(phase_trace,
                                   "copy_done",
                                   "build_native_row_packet",
                                   packet_started,
                                   data_packet.size(),
                                   chunk_count + 1,
                                   row_count,
                                   use_native_row_packet ? "scratchbird.native_rows.v2" : "not_used");
    const std::int64_t execute_started = phase_trace ? ParserPhaseNowNs() : 0;
    const bool use_prepared =
        !data_packet.empty() && CopyPreparedHandleCurrent(copy, session->session());
    auto chunk_result =
        data_packet.empty()
            ? session->RunSblrEnvelope(envelope, false)
            : (use_prepared
                   ? session->RunPreparedSblrEnvelopeForWire(
                         copy.prepared_statement_uuid, envelope, data_packet, false)
                   : session->RunSblrEnvelopeWithDataPacket(envelope, data_packet, false));
    WriteParserPhaseTraceIfEnabled(phase_trace,
                                   "copy_done",
                                   data_packet.empty()
                                       ? "run_sblr_envelope"
                                       : (use_prepared
                                              ? "run_prepared_sblr_with_data_packet"
                                              : "run_sblr_with_data_packet"),
                                   execute_started,
                                   envelope.size() + data_packet.size(),
                                   chunk_count + 1,
                                   row_count,
                                   chunk_result.accepted ? "accepted" : "rejected");
    if (!chunk_result.accepted || chunk_result.messages.has_errors()) {
      const std::string diagnostic_code = FirstDiagnosticCode(
          chunk_result.messages, "SBSQL.COPY.EXECUTION_REJECTED");
      const std::string diagnostic_detail =
          DiagnosticFieldValue(chunk_result.messages, "detail");
      if (!SendError(io,
                     state,
                     "42000",
                     FirstDiagnosticText(chunk_result.messages),
                     diagnostic_detail.empty() ? diagnostic_code
                                               : diagnostic_code + ";" + diagnostic_detail)) {
        return false;
      }
      return SendReady(io, state, ReadyReason::kErrorRecovered);
    }
    ++chunk_count;
    result.statement_family = chunk_result.statement_family;
    result.operation_family = chunk_result.operation_family;
    result.server_operation_id = chunk_result.server_operation_id;
    result.server_row_count += chunk_result.server_row_count == 0 ? row_count
                                                                  : chunk_result.server_row_count;
    result.server_affected_rows += chunk_result.server_affected_rows_present
                                       ? chunk_result.server_affected_rows
                                       : (chunk_result.server_row_count == 0
                                              ? row_count
                                              : chunk_result.server_row_count);
    result.server_affected_rows_present = true;
    if (!chunk_result.server_result_payload.empty()) {
      if (!result.server_result_payload.empty() && result.server_result_payload.back() != '\n') {
        result.server_result_payload.push_back('\n');
      }
      result.server_result_payload += chunk_result.server_result_payload;
    }
  }
  const std::int64_t send_started = phase_trace ? ParserPhaseNowNs() : 0;
  if (!SendPipelineResult(io, session, state, copy_sql, result)) return false;
  WriteParserPhaseTraceIfEnabled(phase_trace,
                                 "copy_done",
                                 "send_pipeline_result",
                                 send_started,
                                 0,
                                 chunk_count,
                                 result.server_row_count,
                                 "sent");
  WriteParserPhaseTraceIfEnabled(phase_trace,
                                 "copy_done",
                                 "total",
                                 total_started,
                                 0,
                                 chunk_count,
                                 result.server_row_count,
                                 copy.native_bulk_ingest ? "native_bulk_ingest"
                                                         : "import_rows");
  return SendReady(io, state, ReadyReason::kCommandComplete);
}

bool HandleCopyFail(ClientIo* io, SbwpSessionState* state, const Frame& frame) {
  (void)frame;
  state->copy_import = CopyImportState{};
  return SendError(io,
                   state,
                   "57014",
                   "SBSQL.COPY.CLIENT_ABORTED",
                   "client aborted the active COPY stream") &&
         SendReady(io, state, ReadyReason::kErrorRecovered);
}

bool HandleStartup(SbsqlTestWireSession* session,
                   const ParserConfig& config,
                   ClientIo* io,
                   SbwpSessionState* state,
                   const Frame& startup_frame) {
  const auto startup = ParseStartupNegotiation(startup_frame.payload);
  if (!startup.ok) {
    state->p1_payloads = startup.p1;
    (void)SendError(io, state, startup.sqlstate, startup.message, startup.detail);
    return !startup.close_after_error;
  }
  state->p1_payloads = startup.p1;
  state->selected_protocol_version = startup.selected_protocol_version;
  state->negotiated_features = startup.negotiated_features;
  state->session_parameters = startup.params;
  const auto& params = startup.params;
  if (!SendFrame(io, state, kAuthRequest, AuthRequestPayload())) return false;

  Frame auth_response;
  if (!ReadFrame(io, &auth_response)) return true;
  if (auth_response.header.msg_type != kAuthResponse) {
    (void)SendError(io, state, "08P01", "expected AUTH_RESPONSE after AUTH_REQUEST");
    return false;
  }

  auto get_param = [&](const std::string& key) -> std::string {
    const auto found = params.find(key);
    return found == params.end() ? std::string{} : found->second;
  };
  AuthCredentialEnvelope credentials;
  credentials.provider_family = "local_password";
  credentials.principal = get_param("user");
  credentials.requested_database = get_param("database");
  if (credentials.requested_database.empty()) {
    credentials.requested_database = config.database_token.empty() ? "default" : config.database_token;
  }
  credentials.requested_language = get_param("language");
  if (credentials.requested_language.empty()) credentials.requested_language = "en";
  credentials.requested_role = get_param("role");
  credentials.application_name = get_param("application_name");
  if (!config.manager_auth_token.empty()) {
    credentials.provider_family = config.manager_auth_provider_family.empty()
                                      ? "security_database_temporary_token"
                                      : config.manager_auth_provider_family;
    if (!config.manager_auth_principal.empty()) credentials.principal = config.manager_auth_principal;
    credentials.credential_evidence_present = true;
    credentials.credential_evidence = "scheme=security_database_temporary_token_v1;principal=" +
                                      credentials.principal + ";token=" +
                                      config.manager_auth_token + ";issuer=manager";
  } else {
    credentials.credential_evidence_present = !auth_response.payload.empty();
    credentials.credential_evidence.assign(reinterpret_cast<const char*>(auth_response.payload.data()),
                                           auth_response.payload.size());
  }

  MessageVectorSet auth_messages;
  if (!session->AuthenticateCredentials(credentials, &auth_messages)) {
    (void)SendError(io,
                    state,
                    "28000",
                    FirstDiagnosticText(auth_messages),
                    FirstDiagnosticCode(auth_messages, "SECURITY.AUTHENTICATION.FAILED"));
    return false;
  }

  state->attachment_id = TextToUuidBytes(session->session().session_uuid);
  if (IsZeroUuid(state->attachment_id)) {
    state->attachment_id = TextToUuidBytes(session->session().connection_uuid);
  }
  if (IsZeroUuid(state->attachment_id)) {
    state->attachment_id = FallbackAttachmentId(credentials.principal);
  }
  state->session_uuid = state->attachment_id;
  state->txn_id = session->session().local_transaction_id;
  state->snapshot_visible_through_local_transaction_id =
      session->session().snapshot_visible_through_local_transaction_id;
  state->catalog_epoch = session->session().catalog_epoch;
  state->security_policy_epoch = session->session().security_policy_epoch;
  state->grant_epoch = session->session().grant_epoch;
  state->descriptor_epoch = session->session().descriptor_epoch;
  state->authenticated_user_uuid = session->session().authenticated_user_uuid;
  state->auth_provider_family = session->session().auth_provider_family.empty()
                                    ? credentials.provider_family
                                    : session->session().auth_provider_family;
  state->principal_claim = session->session().principal_claim.empty()
                               ? credentials.principal
                               : session->session().principal_claim;
  state->language_profile = session->session().language_profile;
  state->language_tag = session->session().language_tag;
  state->default_language_tag = session->session().default_language;
  state->input_syntax_profile = session->session().input_syntax_profile;
  state->input_language_fallback_tag =
      session->session().input_language_fallback_tag;
  state->common_resource_hash = session->session().common_resource_hash;
  state->language_resource_epoch = session->session().language_resource_epoch;
  state->localized_name_epoch = session->session().localized_name_epoch;
  state->message_resource_epoch = session->session().message_resource_epoch;
  state->resource_compatibility_identity =
      session->session().resource_compatibility_identity;
  state->resource_version_identity =
      session->session().resource_version_identity;
  if (state->txn_id == 0) {
    (void)SendError(io,
                    state,
                    "25000",
                    "PARSER_SERVER_IPC.ATTACH_TRANSACTION_REQUIRED",
                    "accepted database attach did not publish the required active transaction");
    return false;
  }
  state->authenticated = true;
  if (!SendFrame(io, state, kAuthOk, AuthOkPayload(*state))) return false;
  if (!SendServerInfo(io, state, config)) return false;
  if (!SendParameterStatus(io, state, StartupParameterStatuses(*state))) return false;
  return SendReady(io, state, ReadyReason::kStartup);
}

} // namespace

bool SbsqlTestWireSession::AuthenticateCredentials(const AuthCredentialEnvelope& credentials,
                                                   MessageVectorSet* messages) {
  if (metrics_) {
    metrics_->SetState(ParserState::kAuthenticating);
    metrics_->Increment("sys.metrics.parsers.auth.attempts_total");
  }
  if (config_.embedded_engine_direct) {
    if (!config_.embedded_auth_bypass_sysarch) {
      if (messages != nullptr) {
        messages->diagnostics.push_back(MakeDiagnostic(
            "SBSQL.EMBEDDED.AUTH_BYPASS_NOT_ENABLED",
            "ERROR",
            "embedded engine direct mode requires the explicit local sysarch authorization bypass",
            "sbp_sbsql.embedded"));
      }
      if (metrics_) {
        metrics_->Increment("sys.metrics.parsers.auth.failures_total");
        metrics_->SetState(ParserState::kIdlePreAuth);
      }
      return false;
    }
    if (embedded_client_ == nullptr) {
      embedded_client_ = std::make_unique<EmbeddedEngineClient>(config_);
    }
    const bool authenticated_direct_requested =
        credentials.credential_evidence_present && !credentials.principal.empty() &&
        credentials.provider_family != "embedded_sysarch";
    if (authenticated_direct_requested) {
      const bool accepted =
          embedded_client_->AuthenticateAndAttach(credentials, &session_, messages);
      if (accepted) {
        if (metrics_) metrics_->SetState(ParserState::kAuthenticated);
        return true;
      }
      if (metrics_) {
        metrics_->Increment("sys.metrics.parsers.auth.failures_total");
        metrics_->SetState(ParserState::kIdlePreAuth);
      }
      return false;
    }
    AuthCredentialEnvelope embedded_credentials = credentials;
    embedded_credentials.provider_family = "embedded_sysarch";
    embedded_credentials.principal = "sysarch";
    embedded_credentials.credential_evidence.clear();
    embedded_credentials.credential_evidence_present = false;
    if (embedded_credentials.requested_database.empty() ||
        embedded_credentials.requested_database == "default") {
      embedded_credentials.requested_database = config_.embedded_database_path.empty()
                                                    ? config_.database_token
                                                    : config_.embedded_database_path;
    }
    const bool accepted =
        embedded_client_->AuthenticateAndAttachSysarch(embedded_credentials, &session_, messages);
    if (accepted) {
      if (metrics_) metrics_->SetState(ParserState::kAuthenticated);
      return true;
    }
    if (metrics_) {
      metrics_->Increment("sys.metrics.parsers.auth.failures_total");
      metrics_->SetState(ParserState::kIdlePreAuth);
    }
    return false;
  }
  if (config_.server_endpoint.empty()) {
    if (messages != nullptr) {
      messages->diagnostics.push_back(MakeDiagnostic(
          "SBSQL.SERVER.UNAVAILABLE",
          "ERROR",
          "authentication requires the assigned sb_server parser IPC endpoint",
          "sbp_sbsql.sbwp"));
    }
    if (metrics_) metrics_->Increment("sys.metrics.parsers.auth.failures_total");
    if (metrics_) metrics_->SetState(ParserState::kIdlePreAuth);
    return false;
  }
  SbpsClient client(config_.server_endpoint);
  const bool accepted = client.AuthenticateAndAttach(credentials, config_, &session_, messages);
  if (accepted) {
    if (metrics_) metrics_->SetState(ParserState::kAuthenticated);
    return true;
  }
  if (metrics_) {
    metrics_->Increment("sys.metrics.parsers.auth.failures_total");
    metrics_->SetState(ParserState::kIdlePreAuth);
  }
  return false;
}

int SbsqlTestWireSession::ServeSbwp(std::intptr_t fd) {
  ClientIo io(fd);
  if (config_.tls_required && !io.StartTls(config_.tls_cert_file, config_.tls_key_file)) {
    if (metrics_) metrics_->SetState(ParserState::kFailed);
    return 1;
  }
  if (metrics_) metrics_->SetState(ParserState::kIdlePreAuth);

  SbwpSessionState state;
  int rc = 0;
  for (;;) {
    Frame frame;
    if (!ReadFrame(&io, &frame)) break;
    const bool frame_compressed = (frame.header.flags & kFrameFlagCompressed) != 0;
    const bool frame_partial = (frame.header.flags & kFrameFlagPartial) != 0;
    const bool frame_final = (frame.header.flags & kFrameFlagFinal) != 0;
    if ((frame.header.flags & ~kFrameFlagKnownMask) != 0 ||
        (frame_compressed && !FeatureNegotiated(state, kFeatureCompression))) {
      (void)SendError(&io,
                      &state,
                      "08P01",
                      frame_compressed
                          ? "SBWP.COMPRESSION.UNNEGOTIATED_FRAME"
                          : "SBWP.HEADER.RESERVED_BITS_SET",
                      "SBWP frame flags failed closed before payload dispatch");
      rc = 1;
      break;
    }
    if (frame_partial || frame_final || state.partial_query_active) {
      if (frame.header.msg_type != kQuery) {
        (void)SendError(&io,
                        &state,
                        "08P01",
                        "SBWP.PARTIAL_QUERY.INVALID_MESSAGE",
                        "partial/final frame continuation is only supported for Query payloads");
        rc = 1;
        break;
      }
      if (!FeatureNegotiated(state, kFeatureStreaming)) {
        (void)SendFeatureNotNegotiated(&io, &state, frame.header.msg_type);
        rc = 1;
        break;
      }
      if (!state.partial_query_active) {
        state.partial_query_payload.clear();
        state.partial_query_active = true;
      }
      if (state.partial_query_payload.size() + frame.payload.size() > kMaxPayloadBytes) {
        state.partial_query_active = false;
        state.partial_query_payload.clear();
        (void)SendError(&io,
                        &state,
                        "54000",
                        "SBWP.PARTIAL_QUERY.PAYLOAD_TOO_LARGE",
                        "partial Query payload exceeds the parser route payload budget");
        rc = 1;
        break;
      }
      state.partial_query_payload.insert(state.partial_query_payload.end(),
                                         frame.payload.begin(),
                                         frame.payload.end());
      if (!frame_final) {
        continue;
      }
      frame.payload = std::move(state.partial_query_payload);
      state.partial_query_payload.clear();
      state.partial_query_active = false;
      frame.header.flags = static_cast<std::uint8_t>(
          frame.header.flags & ~(kFrameFlagPartial | kFrameFlagFinal));
    }
    if (!IsKnownSbwpMessage(frame.header.msg_type)) {
      (void)SendError(&io,
                      &state,
                      "08P01",
                      "NATIVE_WIRE.PROTOCOL_FATAL",
                      "unknown SBWP frame type is not recoverable");
      rc = 1;
      break;
    }
    const auto required_feature = RequiredFeatureForMessage(frame.header.msg_type);
    if (!FeatureNegotiated(state, required_feature)) {
      (void)SendFeatureNotNegotiated(&io, &state, frame.header.msg_type);
      rc = 1;
      break;
    }
    switch (frame.header.msg_type) {
      case kStartup:
        if (!HandleStartup(this, config_, &io, &state, frame)) rc = 1;
        break;
      case kQuery:
        if (!state.authenticated) {
          if (!SendError(&io, &state, "28000", "authentication required") ||
              !SendReady(&io, &state)) rc = 1;
        } else if (!AdmitFrameTransaction(&io, &state, frame, "QUERY")) {
          break;
        } else if (!ExecuteScriptOrSql(this,
                                       &io,
                                       &state,
                                       ParseQueryPayload(frame.payload),
                                       true)) {
          rc = 1;
        }
        break;
      case kParse: {
        state.ready_sent_for_current_operation = false;
        std::string name;
        auto prepared = ParsePreparedStatement(frame.payload, &name);
        if (!prepared) {
          if (!SendError(&io, &state, "08P01", "invalid PARSE payload")) rc = 1;
          break;
        }
        if (state.authenticated && prepared->insert_rowset_plan.has_value()) {
          auto resolved = ResolvePublicNameForWire(prepared->insert_rowset_plan->target_name,
                                                   false,
                                                   "relation");
          if (resolved.resolved) {
            prepared->insert_rowset_plan->target_object_uuid = std::move(resolved.object_uuid);
            CopyImportState prepare_template;
            prepare_template.native_bulk_ingest = true;
            prepare_template.native_bulk_ingest_enabled = true;
            prepare_template.sql = prepared->sql;
            prepare_template.target_object_uuid = prepared->insert_rowset_plan->target_object_uuid;
            const auto prepared_handle =
                PrepareSblrForWire(BuildNativeBulkIngestExecuteEnvelope(prepare_template, 0, 0));
            if (prepared_handle.accepted) {
              prepared->insert_rowset_plan->server_prepared_statement_uuid =
                  prepared_handle.prepared_statement_uuid;
              prepared->insert_rowset_plan->server_operation_id =
                  prepared_handle.operation_id;
            }
          }
        }
        if (!StorePreparedStatement(&state, name, std::move(*prepared))) {
          if (!SendError(&io,
                         &state,
                         "54000",
                         "prepared statement cache limit exceeded",
                         "prepared statement exceeds the per-session server cache budget")) rc = 1;
          break;
        }
        if (!SendFrame(&io, &state, kParseComplete, {})) rc = 1;
        break;
      }
      case kDescribe: {
        state.ready_sent_for_current_operation = false;
        std::size_t off = 4;
        const std::string name = ReadSizedString(frame.payload, &off);
        PreparedStatement statement;
        const auto found = state.statements.find(name);
        if (found != state.statements.end()) statement = found->second;
        if (!SendFrame(&io,
                       &state,
                       kParameterDescription,
                       ParameterDescriptionPayload(statement,
                                                   state.p1_payloads,
                                                   state.negotiated_features))) {
          rc = 1;
        }
        break;
      }
      case kBind: {
        state.ready_sent_for_current_operation = false;
        if (state.p1_payloads && LooksLikeP1ParameterDataBind(frame.payload) &&
            frame.payload[3] == static_cast<std::uint8_t>(
                                    datatypes::WireParameterBindShape::array_bind_rows)) {
          if (!FeatureNegotiated(state, kFeatureArrayBind)) {
            if (!SendFeatureNotNegotiated(&io, &state, frame.header.msg_type)) rc = 1;
            break;
          }
          std::string diagnostic_code;
          auto bound = ParseArrayBindPayload(frame.payload, state.statements, &diagnostic_code);
          if (!bound) {
            if (!SendError(&io,
                           &state,
                           "08P01",
                           diagnostic_code.empty() ? "SBWP.ARRAY_BIND.INVALID" : diagnostic_code,
                           "invalid array-bind parameter data packet")) rc = 1;
            break;
          }
          const std::string portal_name;
          if (!StoreBoundPortal(&state, portal_name, std::move(*bound))) {
            if (!SendError(&io,
                           &state,
                           "54000",
                           "bound portal cache limit exceeded",
                           "array-bind portal exceeds the per-session server cache budget")) rc = 1;
            break;
          }
          if (!SendFrame(&io, &state, kBindComplete, {})) rc = 1;
          break;
        }
        std::string portal_name;
        const auto bound = ParseBindPayload(frame.payload, state.statements, &portal_name);
        if (!bound) {
          if (!SendError(&io, &state, "08P01", "invalid BIND payload")) rc = 1;
          break;
        }
        if (!StoreBoundPortal(&state, portal_name, std::move(*bound))) {
          if (!SendError(&io,
                         &state,
                         "54000",
                         "bound portal cache limit exceeded",
                         "bound portal exceeds the per-session server cache budget")) rc = 1;
          break;
        }
        if (!SendFrame(&io, &state, kBindComplete, {})) rc = 1;
        break;
      }
      case kExecute: {
        state.ready_sent_for_current_operation = false;
        const std::string portal_name = ParsePortalName(frame.payload);
        const bool autocommit_emulation =
            (ParseExecuteFlags(frame.payload) & kExecuteFlagAutocommit) != 0;
        const auto found = state.portals.find(portal_name);
        const std::string sql = found == state.portals.end() ? std::string{} : found->second.sql;
        if (!state.authenticated) {
          if (!SendError(&io, &state, "28000", "authentication required")) rc = 1;
        } else if (found == state.portals.end()) {
          if (!SendError(&io, &state, "34000", "bound portal not found")) rc = 1;
        } else if (!AdmitFrameTransaction(&io, &state, frame, "EXECUTE")) {
          break;
        } else {
          auto rowset_executed = ExecutePreparedInsertRowset(this, &io, &state, found->second, false);
          if (rowset_executed.has_value()) {
            if (!*rowset_executed) rc = 1;
          } else if (!ExecuteSql(this, &io, &state, sql, false, autocommit_emulation)) {
            rc = 1;
          }
        }
        break;
      }
      case kSync:
        if (!state.ready_sent_for_current_operation && !SendReady(&io, &state)) rc = 1;
        state.ready_sent_for_current_operation = false;
        break;
      case kClose:
        state.ready_sent_for_current_operation = false;
        if (!frame.payload.empty()) {
          std::size_t off = 4;
          const std::string name = ReadSizedString(frame.payload, &off);
          if (frame.payload[0] == 'S') {
            RemovePreparedStatement(&state, name);
          } else if (frame.payload[0] == 'P') {
            RemoveBoundPortal(&state, name);
          }
        }
        if (!SendFrame(&io, &state, kCloseComplete, {})) rc = 1;
        break;
      case kTxnBegin:
        if (!ExecuteSql(this, &io, &state, "BEGIN", true)) rc = 1;
        break;
      case kTxnCommit:
        if (!AdmitFrameTransaction(&io, &state, frame, "TXN_COMMIT")) {
          break;
        }
        {
          SbwpTxnCommitRequest request;
          if (!ParseTxnCommitPayload(frame.payload, &request)) {
            if (!SendError(&io,
                           &state,
                           "08P01",
                           "SBWP.COMMIT.IDEMPOTENCY_PAYLOAD_INVALID",
                           "TxnCommit finality/idempotency payload is truncated or unsupported") ||
                !SendReady(&io, &state, ReadyReason::kErrorRecovered)) {
              rc = 1;
            }
            break;
          }
          const bool replay_candidate =
              (request.contract_flags & kTxnCommitFlagHasIdempotencyKey) != 0 &&
              !IsZeroUuid(request.idempotency_key) &&
              FindFinalityRecord(state, request.idempotency_key, {}) != nullptr;
          if (replay_candidate) {
            if (!HandleTxnCommitReplay(&io, &state, request)) rc = 1;
            break;
          }
          if (!ExecuteSql(this, &io, &state, "COMMIT", true, false, &request)) rc = 1;
        }
        break;
      case kTxnStatus:
        if (!state.authenticated) {
          if (!SendError(&io, &state, "28000", "authentication required") ||
              !SendReady(&io, &state, ReadyReason::kErrorRecovered)) rc = 1;
        } else if (!HandleTxnStatus(&io, &state, frame)) {
          rc = 1;
        }
        break;
      case kTxnRollback:
        if (!AdmitFrameTransaction(&io, &state, frame, "TXN_ROLLBACK")) {
          break;
        }
        if (!ExecuteSql(this, &io, &state, "ROLLBACK", true)) rc = 1;
        break;
      case kTxnSavepoint:
      case kTxnRelease:
      case kTxnRollbackTo: {
        const char* operation = frame.header.msg_type == kTxnSavepoint
                                    ? "TXN_SAVEPOINT"
                                    : frame.header.msg_type == kTxnRelease ? "TXN_RELEASE"
                                                                            : "TXN_ROLLBACK_TO";
        const char* prefix = frame.header.msg_type == kTxnSavepoint
                                 ? "SAVEPOINT "
                                 : frame.header.msg_type == kTxnRelease ? "RELEASE SAVEPOINT "
                                                                         : "ROLLBACK TO SAVEPOINT ";
        if (!AdmitFrameTransaction(&io, &state, frame, operation)) {
          break;
        }
        const auto sql = SavepointSqlFromFrame(frame, prefix);
        if (!sql.has_value()) {
          if (!SendError(&io,
                         &state,
                         "08P01",
                         "SBWP.SAVEPOINT.INVALID_PAYLOAD",
                         "savepoint frames require one simple identifier payload") ||
              !SendReady(&io, &state, ReadyReason::kErrorRecovered)) {
            rc = 1;
          }
          break;
        }
        if (!ExecuteSql(this, &io, &state, *sql, true)) rc = 1;
        break;
      }
      case kCancel:
        if (!HandleCancel(&io, &state, frame)) rc = 1;
        break;
      case kPing:
        if (!SendFrame(&io, &state, kPong, frame.payload)) rc = 1;
        break;
      case kResetSession:
        if (!HandleResetSession(this, &io, &state, frame)) rc = 1;
        break;
      case kReauth:
        if (!HandleReauth(&io, &state, frame)) rc = 1;
        break;
      case kTraceContext:
        if (!HandleTraceContext(&io, &state, frame)) rc = 1;
        break;
      case kSubscribe:
        if (!HandleSubscription(&io, &state, true)) rc = 1;
        break;
      case kUnsubscribe:
        if (!HandleSubscription(&io, &state, false)) rc = 1;
        break;
      case kSetOption: {
        std::string option_name;
        std::string option_value;
        if (!ParseSetOptionPayload(frame.payload, &option_name, &option_value)) {
          if (!SendError(&io,
                         &state,
                         "08P01",
                         "SBWP.SET_OPTION.INVALID_PAYLOAD",
                         "SetOption requires one non-empty option name and one bounded value") ||
              !SendReady(&io, &state, ReadyReason::kErrorRecovered)) {
            rc = 1;
          }
          break;
        }
        if (option_value.empty()) {
          state.session_parameters.erase(option_name);
        } else {
          state.session_parameters[option_name] = option_value;
        }
        if (!SendParameterStatus(&io,
                                 &state,
                                 {{"session.option", "updated"},
                                  {option_name, option_value}}) ||
            !SendReady(&io, &state, ReadyReason::kStateChange)) {
          rc = 1;
        }
        break;
      }
      case kCopyData:
        if (!AdmitFrameTransaction(&io, &state, frame, "COPY_DATA")) {
          break;
        }
        if (!HandleCopyData(this, &io, &state, frame)) rc = 1;
        break;
      case kCopyDone:
        if (!AdmitFrameTransaction(&io, &state, frame, "COPY_DONE")) {
          break;
        }
        if (!HandleCopyDone(this, &io, &state)) rc = 1;
        break;
      case kCopyFail:
        if (!HandleCopyFail(&io, &state, frame)) rc = 1;
        break;
      case kSblrExecute:
        if (!state.authenticated) {
          if (!SendError(&io, &state, "28000", "authentication required") ||
              !SendReady(&io, &state, ReadyReason::kErrorRecovered)) rc = 1;
        } else if (!AdmitFrameTransaction(&io, &state, frame, "SBLR_EXECUTE")) {
          break;
        } else if (!ExecuteNativeSblrFrame(this, &io, &state, frame)) {
          rc = 1;
        }
        break;
      case kStreamControl:
      case kLobClose:
        if (!SendUnsupportedFeature(&io, &state, "native extension frame") ||
            !SendReady(&io, &state, ReadyReason::kErrorRecovered)) {
          rc = 1;
        }
        break;
      case kTerminate:
        rc = 0;
        goto done;
      default:
        if (!SendError(&io, &state, "08P01", "unsupported SBWP frame") ||
            !SendReady(&io, &state)) rc = 1;
        break;
    }
    if (rc != 0) break;
  }

done:
  if (session_.authenticated && HasExecutionRoute()) {
    MessageVectorSet disconnect_messages;
    (void)DisconnectExecutionRoute(&disconnect_messages);
  }
  if (metrics_) metrics_->SetState(rc == 0 ? ParserState::kDisconnected : ParserState::kFailed);
  return rc;
}

} // namespace scratchbird::parser::sbsql
