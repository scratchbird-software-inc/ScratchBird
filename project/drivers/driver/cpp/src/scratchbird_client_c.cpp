// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/client/scratchbird_client.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "scratchbird/client/circuit_breaker.h"
#include "scratchbird/client/connection.h"
#include "scratchbird/client/keepalive.h"
#include "scratchbird/client/leak_detector.h"
#include "scratchbird/client/metadata.h"
#include "scratchbird/client/telemetry.h"
#include "scratchbird/client/driver_config.h"
#include "scratchbird/core/error_context.h"
#include "scratchbird/core/status.h"
#include "scratchbird/core/type_extractor.h"
#include "scratchbird/protocol/sbwp_protocol.h"

struct sb_connection {
    struct NotificationListener {
        sb_notification_listener_fn fn{nullptr};
        void* user_data{nullptr};
    };

    scratchbird::client::NetworkClient client;
    scratchbird::client::NetworkClientConfig config;
    scratchbird::client::CircuitBreaker circuit_breaker;
    scratchbird::client::TelemetryCollector telemetry;
    scratchbird::client::KeepaliveManager keepalive_manager;
    scratchbird::client::KeepaliveTracker* keepalive_tracker{nullptr};
    scratchbird::client::LeakDetector leak_detector;
    std::unique_ptr<scratchbird::client::LeakDetectionGuard> leak_guard;
    std::string connection_id;
    std::deque<scratchbird::client::NetworkClient::Notification> notification_queue;
    std::unordered_map<uint64_t, NotificationListener> notification_listeners;
    uint64_t next_listener_id{1};
    std::mutex notification_mutex;
};

struct sb_prepared {
    sb_connection* conn{nullptr};
    scratchbird::client::NetworkPreparedStatement stmt;
    std::vector<std::string> param_names;
};

struct sb_result {
    scratchbird::client::NetworkResultSet results;
    size_t row_index{0};
    std::vector<std::string> column_names;
};

namespace {

constexpr int32_t kDaysFrom1970To2000 = 10957;
constexpr int64_t kMicrosPerSecond = 1000000LL;
constexpr int64_t kMicrosPerDay = 86400LL * kMicrosPerSecond;
constexpr int64_t kMicrosFrom1970To2000 = kMicrosPerDay * kDaysFrom1970To2000;
constexpr const char* kAbiMemoryAuthorityScope =
    "abi_memory_ownership_evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority";

void set_error(sb_error* err, sb_error_code code, const std::string& message);

struct AbiAllocationRecord {
    size_t bytes{0};
    std::string purpose;
    uint64_t sequence{0};
};

std::mutex& abiAllocationMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<void*, AbiAllocationRecord>& abiAllocations() {
    static std::unordered_map<void*, AbiAllocationRecord> allocations;
    return allocations;
}

uint64_t& abiAllocationSequence() {
    static uint64_t sequence = 0;
    return sequence;
}

void copyAbiText(char* target, size_t target_size, const std::string& value) {
    if (!target || target_size == 0) {
        return;
    }
    std::snprintf(target, target_size, "%s", value.c_str());
}

bool trackAbiAllocation(void* pointer, size_t bytes, std::string purpose) {
    if (!pointer) {
        return false;
    }
    std::lock_guard<std::mutex> lock(abiAllocationMutex());
    try {
        abiAllocations()[pointer] = AbiAllocationRecord{bytes, std::move(purpose), ++abiAllocationSequence()};
    } catch (...) {
        return false;
    }
    return true;
}

bool untrackAbiAllocation(void* pointer, AbiAllocationRecord* record) {
    std::lock_guard<std::mutex> lock(abiAllocationMutex());
    auto it = abiAllocations().find(pointer);
    if (it == abiAllocations().end()) {
        return false;
    }
    if (record) {
        *record = it->second;
    }
    abiAllocations().erase(it);
    return true;
}

size_t trackedAbiAllocationCount() {
    std::lock_guard<std::mutex> lock(abiAllocationMutex());
    return abiAllocations().size();
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (unsigned char ch : value) {
        switch (ch) {
            case '\"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u00"
                        << "0123456789abcdef"[(ch >> 4) & 0x0F]
                        << "0123456789abcdef"[ch & 0x0F];
                } else {
                    out << static_cast<char>(ch);
                }
        }
    }
    return out.str();
}

std::string hexEncode(const uint8_t* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0x0F]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

std::string hexEncode(const std::vector<uint8_t>& data) {
    return hexEncode(data.data(), data.size());
}

std::string trimWhitespace(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string quoteSqlLiteral(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('\'');
    for (char ch : value) {
        out.push_back(ch);
        if (ch == '\'') {
            out.push_back('\'');
        }
    }
    out.push_back('\'');
    return out;
}

bool buildPreparedTransactionSql(const char* verb,
                                 const char* global_transaction_id,
                                 std::string& sql_out,
                                 sb_error* err) {
    if (!verb) {
        set_error(err, SB_ERR_NULL_POINTER, "Prepared-transaction verb is required");
        return false;
    }
    if (!global_transaction_id) {
        set_error(err, SB_ERR_NULL_POINTER, "Global transaction id is required");
        return false;
    }
    const std::string gid = trimWhitespace(global_transaction_id);
    if (gid.empty()) {
        set_error(err, SB_ERR_SYNTAX, "Global transaction id is required");
        return false;
    }
    sql_out = std::string(verb) + " " + quoteSqlLiteral(gid);
    return true;
}

int executeTransactionControlSql(sb_connection* conn,
                                 const std::string& sql,
                                 sb_error* err) {
    sb_result* result = sb_execute(conn, sql.c_str(), err);
    if (!result) {
        return err ? static_cast<int>(err->code) : SB_ERR_UNKNOWN;
    }
    sb_result_free(result);
    set_error(err, SB_OK, "");
    return SB_OK;
}

char* allocateCString(const std::string& value,
                      sb_error* err,
                      const char* oom_message,
                      const char* purpose = "driver_owned_c_string") {
    char* out = static_cast<char*>(std::malloc(value.size() + 1));
    if (!out) {
        set_error(err, SB_ERR_OUT_OF_MEMORY, oom_message ? oom_message : "Out of memory");
        return nullptr;
    }
    std::memcpy(out, value.c_str(), value.size() + 1);
    if (!trackAbiAllocation(out, value.size() + 1, purpose ? purpose : "driver_owned_c_string")) {
        std::free(out);
        set_error(err, SB_ERR_OUT_OF_MEMORY, "Out of memory tracking driver-owned ABI allocation");
        return nullptr;
    }
    set_error(err, SB_OK, "");
    return out;
}

std::string authMethodSurfaceToJson(const scratchbird::client::AuthMethodSurface& value) {
    std::ostringstream out;
    out << "{"
        << "\"wire_method\":\"" << jsonEscape(value.wire_method) << "\","
        << "\"plugin_method_id\":\"" << jsonEscape(value.plugin_method_id) << "\","
        << "\"executable_locally\":" << (value.executable_locally ? "true" : "false") << ","
        << "\"broker_required\":" << (value.broker_required ? "true" : "false")
        << "}";
    return out.str();
}

std::string authProbeResultToJson(const scratchbird::client::AuthProbeResult& value) {
    std::ostringstream out;
    out << "{"
        << "\"reachable\":" << (value.reachable ? "true" : "false") << ","
        << "\"ingress_mode\":\"" << jsonEscape(value.ingress_mode) << "\","
        << "\"resolved_host\":\"" << jsonEscape(value.resolved_host) << "\","
        << "\"resolved_port\":" << value.resolved_port << ","
        << "\"admitted_methods\":[";
    for (size_t i = 0; i < value.admitted_methods.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << authMethodSurfaceToJson(value.admitted_methods[i]);
    }
    out << "],"
        << "\"required_method\":\"" << jsonEscape(value.required_method) << "\","
        << "\"required_plugin_method_id\":\"" << jsonEscape(value.required_plugin_method_id) << "\","
        << "\"additional_continuation_possible\":"
        << (value.additional_continuation_possible ? "true" : "false")
        << "}";
    return out.str();
}

std::string resolvedAuthContextToJson(const scratchbird::client::ResolvedAuthContext& value) {
    std::ostringstream out;
    out << "{"
        << "\"ingress_mode\":\"" << jsonEscape(value.ingress_mode) << "\","
        << "\"resolved_auth_method\":\"" << jsonEscape(value.resolved_auth_method) << "\","
        << "\"resolved_auth_plugin_id\":\"" << jsonEscape(value.resolved_auth_plugin_id) << "\","
        << "\"manager_authenticated\":" << (value.manager_authenticated ? "true" : "false") << ","
        << "\"attached\":" << (value.attached ? "true" : "false")
        << "}";
    return out.str();
}

std::string quoteSqlIdentifier(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (char ch : value) {
        if (ch == '"') {
            out.push_back('"');
        }
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

void set_error(sb_error* err, sb_error_code code, const std::string& message) {
    if (!err) {
        return;
    }
    err->code = code;
    std::snprintf(err->message, sizeof(err->message), "%s", message.c_str());
}

std::string toLowerAscii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string trimAscii(const std::string& value) {
    size_t start = 0;
    while (start < value.size() &&
           std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    size_t end = value.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string decodeTextColumnValue(const scratchbird::protocol::ColumnValue& value) {
    if (value.is_null || value.data.empty()) {
        return "";
    }

    const uint8_t* ptr = value.data.data();
    size_t len = value.data.size();
    if (len >= 4) {
        const uint32_t payload_len = static_cast<uint32_t>(value.data[0]) |
            (static_cast<uint32_t>(value.data[1]) << 8) |
            (static_cast<uint32_t>(value.data[2]) << 16) |
            (static_cast<uint32_t>(value.data[3]) << 24);
        if (payload_len <= len - 4) {
            ptr += 4;
            len = payload_len;
        }
    }

    return trimAscii(std::string(reinterpret_cast<const char*>(ptr), len));
}

int findSchemaNameColumnIndex(const sb_result* result) {
    if (!result) {
        return -1;
    }

    static const char* kCandidates[] = {
        "schema_name",
        "table_schem",
        "table_schema",
        "schema",
    };

    for (size_t i = 0; i < result->column_names.size(); ++i) {
        const std::string normalized = toLowerAscii(result->column_names[i]);
        for (const char* candidate : kCandidates) {
            if (normalized == candidate) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

sb_error_code map_status(scratchbird::core::Status status) {
    using scratchbird::core::Status;
    switch (status) {
        case Status::OK:
            return SB_OK;
        case Status::CONNECTION_FAILURE:
        case Status::CONNECTION_DOES_NOT_EXIST:
            return SB_ERR_CONNECTION_FAILED;
        case Status::INVALID_PASSWORD:
        case Status::INVALID_AUTHORIZATION:
            return SB_ERR_AUTH_FAILED;
        case Status::PROTOCOL_VIOLATION:
            return SB_ERR_PROTOCOL;
        case Status::SYNTAX_ERROR:
            return SB_ERR_SYNTAX;
        case Status::CONSTRAINT_VIOLATION:
            return SB_ERR_CONSTRAINT;
        case Status::TYPE_MISMATCH:
        case Status::DATATYPE_MISMATCH:
            return SB_ERR_TYPE_MISMATCH;
        case Status::DEADLOCK:
            return SB_ERR_DEADLOCK;
        case Status::SERIALIZATION_FAILURE:
            return SB_ERR_SERIALIZATION;
        case Status::READ_ONLY_TRANSACTION:
        case Status::INVALID_TRANSACTION_STATE:
        case Status::LOCK_CONFLICT:
            return SB_ERR_TXN_CONFLICT;
        case Status::TRANSACTION_ABORTED:
            return SB_ERR_TXN_ABORTED;
        case Status::NO_ACTIVE_TRANSACTION:
            return SB_ERR_NO_ACTIVE_TXN;
        case Status::LOCK_TIMEOUT:
            return SB_ERR_TIMEOUT;
        case Status::OOM:
            return SB_ERR_OUT_OF_MEMORY;
        case Status::DISK_FULL:
            return SB_ERR_DISK_FULL;
        case Status::TOO_MANY_CONNECTIONS:
            return SB_ERR_TOO_MANY_CONNECTIONS;
        case Status::INVALID_ARGUMENT:
            return SB_ERR_INVALID_PARAM;
        case Status::NOT_IMPLEMENTED:
        case Status::NOT_SUPPORTED:
            return SB_ERR_NOT_IMPLEMENTED;
        case Status::OBJECT_IN_USE:
            return SB_ERR_RESOURCE_BUSY;
        default:
            return SB_ERR_UNKNOWN;
    }
}

struct OperationSpan {
    std::unique_ptr<scratchbird::client::SpanContext> span;
};

int drain_notifications(sb_connection* conn) {
    if (!conn) {
        return 0;
    }
    std::vector<scratchbird::client::NetworkClient::Notification> drained;
    conn->client.drainNotifications(drained);
    if (drained.empty()) {
        return 0;
    }

    std::vector<sb_connection::NotificationListener> listeners;
    {
        std::lock_guard<std::mutex> lock(conn->notification_mutex);
        listeners.reserve(conn->notification_listeners.size());
        for (const auto& pair : conn->notification_listeners) {
            listeners.push_back(pair.second);
        }
        for (const auto& item : drained) {
            conn->notification_queue.push_back(item);
        }
    }

    if (!listeners.empty()) {
        for (const auto& item : drained) {
            const int has_row_id = item.row_id != 0 ? 1 : 0;
            for (const auto& listener : listeners) {
                if (!listener.fn) {
                    continue;
                }
                try {
                    listener.fn(item.channel.c_str(),
                                item.payload.data(),
                                item.payload.size(),
                                item.process_id,
                                item.change_type,
                                item.row_id,
                                has_row_id,
                                listener.user_data);
                } catch (...) {
                }
            }
        }
    }

    return static_cast<int>(drained.size());
}

bool start_operation(sb_connection* conn, const char* op_name, const char* sql, OperationSpan& ctx, sb_error* err) {
    if (!conn->circuit_breaker.AllowRequest()) {
        set_error(err, SB_ERR_CONNECTION_FAILED, "Circuit breaker is OPEN");
        return false;
    }
    ctx.span = conn->telemetry.StartSpan(op_name ? op_name : "operation");
    if (ctx.span && sql) {
        ctx.span->WithAttribute("db.statement",
            scratchbird::client::TelemetryCollector::SanitizeQuery(sql));
    }
    return true;
}

void end_operation(sb_connection* conn, OperationSpan& ctx, bool success) {
    if (success) {
        conn->circuit_breaker.RecordSuccess();
    } else {
        conn->circuit_breaker.RecordFailure();
    }
    if (conn->keepalive_tracker) {
        conn->keepalive_tracker->MarkActive();
    }
    if (ctx.span) {
        conn->telemetry.EndSpan(*ctx.span, success);
    }
    drain_notifications(conn);
}

static std::atomic<uint64_t> kConnectionCounter{0};

sb_type map_type_oid(uint32_t type_oid) {
    using namespace scratchbird::protocol;
    switch (type_oid) {
        case kOidBool:
            return SB_TYPE_BOOLEAN;
        case kOidInt2:
            return SB_TYPE_SMALLINT;
        case kOidInt4:
            return SB_TYPE_INTEGER;
        case kOidInt8:
            return SB_TYPE_BIGINT;
        case kOidFloat4:
            return SB_TYPE_REAL;
        case kOidFloat8:
            return SB_TYPE_DOUBLE;
        case kOidNumeric:
            return SB_TYPE_DECIMAL;
        case kOidJsonb:
            return SB_TYPE_JSONB;
        case kOidChar:
        case kOidBpChar:
            return SB_TYPE_CHAR;
        case kOidVarchar:
            return SB_TYPE_VARCHAR;
        case kOidText:
            return SB_TYPE_TEXT;
        case kOidXml:
            return SB_TYPE_XML;
        case kOidTsVector:
            return SB_TYPE_TSVECTOR;
        case kOidTsQuery:
            return SB_TYPE_TSQUERY;
        case kOidBytea:
            return SB_TYPE_BLOB;
        case kOidMoney:
            return SB_TYPE_MONEY;
        case kOidDate:
            return SB_TYPE_DATE;
        case kOidTime:
            return SB_TYPE_TIME;
        case kOidTimetz:
            return SB_TYPE_TIME_TZ;
        case kOidTimestamp:
            return SB_TYPE_TIMESTAMP;
        case kOidTimestamptz:
            return SB_TYPE_TIMESTAMP_TZ;
        case kOidInterval:
            return SB_TYPE_INTERVAL;
        case kOidUuid:
            return SB_TYPE_UUID;
        case kOidJson:
            return SB_TYPE_JSON;
        case kOidPoint:
        case kOidLseg:
        case kOidPath:
        case kOidBox:
        case kOidPolygon:
        case kOidLine:
        case kOidCircle:
            return SB_TYPE_GEOMETRY;
        case kOidInet:
            return SB_TYPE_INET;
        case kOidCidr:
            return SB_TYPE_CIDR;
        case kOidMacaddr:
        case kOidMacaddr8:
            return SB_TYPE_MACADDR;
        case kOidSbVector:
            return SB_TYPE_VECTOR;
        case kOidRecord:
            return SB_TYPE_COMPOSITE;
        case kOidInt4Array:
        case kOidTextArray:
            return SB_TYPE_ARRAY;
        case kOidInt4Range:
        case kOidInt8Range:
        case kOidNumRange:
        case kOidTsRange:
        case kOidTstzRange:
        case kOidDateRange:
            return SB_TYPE_RANGE;
        default:
            return SB_TYPE_UNKNOWN;
    }
}

uint32_t map_sb_type_to_oid(sb_type type) {
    using namespace scratchbird::protocol;
    switch (type) {
        case SB_TYPE_BOOLEAN:
            return kOidBool;
        case SB_TYPE_SMALLINT:
            return kOidInt2;
        case SB_TYPE_INTEGER:
            return kOidInt4;
        case SB_TYPE_BIGINT:
            return kOidInt8;
        case SB_TYPE_REAL:
            return kOidFloat4;
        case SB_TYPE_DOUBLE:
            return kOidFloat8;
        case SB_TYPE_DECIMAL:
            return kOidNumeric;
        case SB_TYPE_JSONB:
            return kOidJsonb;
        case SB_TYPE_CHAR:
            return kOidBpChar;
        case SB_TYPE_VARCHAR:
        case SB_TYPE_TEXT:
            return kOidText;
        case SB_TYPE_XML:
            return kOidXml;
        case SB_TYPE_TSVECTOR:
            return kOidTsVector;
        case SB_TYPE_TSQUERY:
            return kOidTsQuery;
        case SB_TYPE_BLOB:
            return kOidBytea;
        case SB_TYPE_MONEY:
            return kOidMoney;
        case SB_TYPE_DATE:
            return kOidDate;
        case SB_TYPE_TIME:
            return kOidTime;
        case SB_TYPE_TIME_TZ:
            return kOidTimetz;
        case SB_TYPE_TIMESTAMP:
            return kOidTimestamp;
        case SB_TYPE_TIMESTAMP_TZ:
            return kOidTimestamptz;
        case SB_TYPE_INTERVAL:
            return kOidInterval;
        case SB_TYPE_UUID:
            return kOidUuid;
        case SB_TYPE_JSON:
            return kOidJson;
        case SB_TYPE_GEOMETRY:
            return kOidPoint;
        case SB_TYPE_ARRAY:
            return kOidTextArray;
        case SB_TYPE_COMPOSITE:
            return kOidRecord;
        case SB_TYPE_RANGE:
            return kOidInt4Range;
        case SB_TYPE_VECTOR:
            return kOidSbVector;
        case SB_TYPE_INET:
            return kOidInet;
        case SB_TYPE_CIDR:
            return kOidCidr;
        case SB_TYPE_MACADDR:
            return kOidMacaddr;
        default:
            return 0;
    }
}

uint32_t resolve_type_oid(const sb_value* value) {
    if (!value) {
        return 0;
    }
    if (value->type_oid != 0) {
        return value->type_oid;
    }
    return map_sb_type_to_oid(value->type);
}

void decode_date(int32_t days_since_2000, sb_value* value) {
    int64_t days_since_epoch = static_cast<int64_t>(days_since_2000) + kDaysFrom1970To2000;
    value->data.date_val.year = scratchbird::core::TypeExtractor::extractYear(days_since_epoch);
    value->data.date_val.month = scratchbird::core::TypeExtractor::extractMonth(days_since_epoch);
    value->data.date_val.day = scratchbird::core::TypeExtractor::extractDay(days_since_epoch);
}

void decode_time(int64_t micros, sb_value* value) {
    value->data.time_val.hour = scratchbird::core::TypeExtractor::extractHour(micros);
    value->data.time_val.minute = scratchbird::core::TypeExtractor::extractMinute(micros);
    value->data.time_val.second = scratchbird::core::TypeExtractor::extractSecond(micros);
    value->data.time_val.microsecond = scratchbird::core::TypeExtractor::extractMicrosecond(micros);
}

void decode_timestamp(int64_t micros, sb_value* value) {
    value->data.timestamp_val.epoch_microseconds = micros;
    value->data.timestamp_val.tz_offset_seconds = 0;
}

void parse_named_params(const std::string& sql, std::vector<std::string>& names) {
    names.clear();
    bool in_string = false;
    for (size_t i = 0; i + 1 < sql.size(); ++i) {
        char ch = sql[i];
        if (ch == '\'') {
            in_string = !in_string;
            continue;
        }
        if (in_string) {
            continue;
        }
        if ((ch == ':' || ch == '@') && std::isalpha(static_cast<unsigned char>(sql[i + 1]))) {
            size_t j = i + 1;
            while (j < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[j])) || sql[j] == '_')) {
                ++j;
            }
            names.emplace_back(sql.substr(i + 1, j - i - 1));
            i = j;
        }
    }
}

int apply_bind_value(scratchbird::client::NetworkPreparedStatement& stmt, size_t index, const sb_value* value) {
    if (!value) {
        return SB_ERR_NULL_POINTER;
    }
    if (value->is_null) {
        stmt.setNull(index, resolve_type_oid(value));
        return SB_OK;
    }
    switch (value->type) {
        case SB_TYPE_BOOLEAN:
            stmt.setBool(index, value->data.boolean_val != 0);
            return SB_OK;
        case SB_TYPE_SMALLINT:
            stmt.setInt16(index, value->data.smallint_val);
            return SB_OK;
        case SB_TYPE_INTEGER:
            stmt.setInt32(index, value->data.integer_val);
            return SB_OK;
        case SB_TYPE_BIGINT:
            stmt.setInt64(index, value->data.bigint_val);
            return SB_OK;
        case SB_TYPE_REAL:
            stmt.setFloat(index, value->data.real_val);
            return SB_OK;
        case SB_TYPE_DOUBLE:
            stmt.setDouble(index, value->data.double_val);
            return SB_OK;
        case SB_TYPE_CHAR:
        case SB_TYPE_VARCHAR:
        case SB_TYPE_TEXT:
        case SB_TYPE_JSON:
        case SB_TYPE_JSONB:
        case SB_TYPE_XML:
        case SB_TYPE_TSVECTOR:
        case SB_TYPE_TSQUERY:
        case SB_TYPE_DECIMAL:
            stmt.setString(index,
                           std::string(value->data.string_val.data, value->data.string_val.length),
                           resolve_type_oid(value));
            return SB_OK;
        case SB_TYPE_ARRAY:
        case SB_TYPE_VECTOR:
        case SB_TYPE_INET:
        case SB_TYPE_CIDR:
        case SB_TYPE_MACADDR:
            if (value->type == SB_TYPE_ARRAY && resolve_type_oid(value) == 0) {
                return SB_ERR_INVALID_PARAM;
            }
            if (!value->data.binary_val.data && value->data.binary_val.length > 0) {
                return SB_ERR_NULL_POINTER;
            }
            stmt.setBinary(index,
                           value->data.binary_val.data,
                           value->data.binary_val.length,
                           resolve_type_oid(value),
                           false);
            return SB_OK;
        case SB_TYPE_COMPOSITE:
        case SB_TYPE_RANGE:
        case SB_TYPE_UNKNOWN: {
            uint32_t type_oid = resolve_type_oid(value);
            if ((value->type == SB_TYPE_RANGE || value->type == SB_TYPE_ARRAY) && type_oid == 0) {
                return SB_ERR_INVALID_PARAM;
            }
            stmt.setBinary(index,
                           value->data.binary_val.data,
                           value->data.binary_val.length,
                           type_oid,
                           false);
            return SB_OK;
        }
        case SB_TYPE_GEOMETRY:
            stmt.setBinary(index,
                           value->data.binary_val.data,
                           value->data.binary_val.length,
                           resolve_type_oid(value),
                           false);
            return SB_OK;
        case SB_TYPE_BLOB:
            stmt.setBytes(index,
                          reinterpret_cast<const uint8_t*>(value->data.binary_val.data),
                          value->data.binary_val.length);
            return SB_OK;
        case SB_TYPE_MONEY: {
            int64_t cents = value->data.money_val;
            stmt.setBinary(index,
                           reinterpret_cast<const uint8_t*>(&cents),
                           sizeof(cents),
                           resolve_type_oid(value),
                           false);
            return SB_OK;
        }
        case SB_TYPE_DATE: {
            int32_t days = scratchbird::core::TypeExtractor::ymdToDays(
                value->data.date_val.year,
                value->data.date_val.month,
                value->data.date_val.day);
            int32_t days_since_2000 = days - kDaysFrom1970To2000;
            stmt.setDate(index, days_since_2000);
            return SB_OK;
        }
        case SB_TYPE_TIME:
        case SB_TYPE_TIME_TZ: {
            int64_t micros = (static_cast<int64_t>(value->data.time_val.hour) * 3600 +
                              static_cast<int64_t>(value->data.time_val.minute) * 60 +
                              static_cast<int64_t>(value->data.time_val.second)) * 1000000LL +
                             value->data.time_val.microsecond;
            stmt.setTime(index, micros);
            return SB_OK;
        }
        case SB_TYPE_INTERVAL: {
            int64_t micros = value->data.interval_val.micros;
            int32_t days = value->data.interval_val.days;
            int32_t months = value->data.interval_val.months;
            uint8_t buf[16];
            std::memcpy(buf, &micros, sizeof(micros));
            std::memcpy(buf + 8, &days, sizeof(days));
            std::memcpy(buf + 12, &months, sizeof(months));
            stmt.setBinary(index, buf, sizeof(buf), resolve_type_oid(value), false);
            return SB_OK;
        }
        case SB_TYPE_TIMESTAMP:
        case SB_TYPE_TIMESTAMP_TZ:
            stmt.setTimestamp(index, value->data.timestamp_val.epoch_microseconds);
            return SB_OK;
        case SB_TYPE_UUID: {
            std::vector<uint8_t> data(value->data.uuid_val.bytes,
                                      value->data.uuid_val.bytes + 16);
            stmt.setUUID(index, data);
            return SB_OK;
        }
        default:
            return SB_ERR_INVALID_PARAM;
    }
}

int build_param_value(const sb_value* value, scratchbird::protocol::ParamValue& out) {
    if (!value) {
        return SB_ERR_NULL_POINTER;
    }
    out = scratchbird::protocol::ParamValue{};
    out.format = scratchbird::protocol::kFormatBinary;
    out.type_oid = resolve_type_oid(value);
    if (value->is_null || value->type == SB_TYPE_NULL) {
        out.is_null = true;
        return SB_OK;
    }
    if ((value->type == SB_TYPE_RANGE || value->type == SB_TYPE_ARRAY) && out.type_oid == 0) {
        return SB_ERR_INVALID_PARAM;
    }
    switch (value->type) {
        case SB_TYPE_BOOLEAN:
            out.data = { static_cast<uint8_t>(value->data.boolean_val != 0) };
            return SB_OK;
        case SB_TYPE_SMALLINT: {
            int16_t v = value->data.smallint_val;
            out.data.resize(2);
            out.data[0] = static_cast<uint8_t>(v & 0xFF);
            out.data[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
            return SB_OK;
        }
        case SB_TYPE_INTEGER: {
            int32_t v = value->data.integer_val;
            out.data.resize(4);
            out.data[0] = static_cast<uint8_t>(v & 0xFF);
            out.data[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
            out.data[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
            out.data[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
            return SB_OK;
        }
        case SB_TYPE_BIGINT: {
            int64_t v = value->data.bigint_val;
            out.data.resize(8);
            for (size_t i = 0; i < 8; ++i) {
                out.data[i] = static_cast<uint8_t>((static_cast<uint64_t>(v) >> (8 * i)) & 0xFF);
            }
            return SB_OK;
        }
        case SB_TYPE_REAL: {
            uint32_t bits = 0;
            std::memcpy(&bits, &value->data.real_val, sizeof(bits));
            out.data.resize(4);
            out.data[0] = static_cast<uint8_t>(bits & 0xFF);
            out.data[1] = static_cast<uint8_t>((bits >> 8) & 0xFF);
            out.data[2] = static_cast<uint8_t>((bits >> 16) & 0xFF);
            out.data[3] = static_cast<uint8_t>((bits >> 24) & 0xFF);
            return SB_OK;
        }
        case SB_TYPE_DOUBLE: {
            uint64_t bits = 0;
            std::memcpy(&bits, &value->data.double_val, sizeof(bits));
            out.data.resize(8);
            for (size_t i = 0; i < 8; ++i) {
                out.data[i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xFF);
            }
            return SB_OK;
        }
        case SB_TYPE_CHAR:
        case SB_TYPE_VARCHAR:
        case SB_TYPE_TEXT:
        case SB_TYPE_JSON:
        case SB_TYPE_JSONB:
        case SB_TYPE_XML:
        case SB_TYPE_TSVECTOR:
        case SB_TYPE_TSQUERY:
        case SB_TYPE_DECIMAL: {
            auto bytes = reinterpret_cast<const uint8_t*>(value->data.string_val.data);
            out.data.assign(bytes, bytes + value->data.string_val.length);
            return SB_OK;
        }
        case SB_TYPE_ARRAY:
        case SB_TYPE_VECTOR:
        case SB_TYPE_INET:
        case SB_TYPE_CIDR:
        case SB_TYPE_MACADDR: {
            if (!value->data.binary_val.data && value->data.binary_val.length > 0) {
                return SB_ERR_NULL_POINTER;
            }
            if (value->data.binary_val.data && value->data.binary_val.length > 0) {
                out.data.assign(value->data.binary_val.data,
                                value->data.binary_val.data + value->data.binary_val.length);
            }
            return SB_OK;
        }
        case SB_TYPE_COMPOSITE:
        case SB_TYPE_RANGE:
        case SB_TYPE_UNKNOWN: {
            if (value->data.binary_val.data && value->data.binary_val.length > 0) {
                out.data.assign(value->data.binary_val.data,
                                value->data.binary_val.data + value->data.binary_val.length);
            }
            return SB_OK;
        }
        case SB_TYPE_GEOMETRY: {
            if (value->data.binary_val.data && value->data.binary_val.length > 0) {
                out.data.assign(value->data.binary_val.data,
                                value->data.binary_val.data + value->data.binary_val.length);
            }
            return SB_OK;
        }
        case SB_TYPE_BLOB: {
            if (value->data.binary_val.data && value->data.binary_val.length > 0) {
                out.data.assign(value->data.binary_val.data,
                                value->data.binary_val.data + value->data.binary_val.length);
            }
            return SB_OK;
        }
        case SB_TYPE_MONEY: {
            int64_t cents = value->data.money_val;
            out.data.resize(8);
            std::memcpy(out.data.data(), &cents, sizeof(cents));
            return SB_OK;
        }
        case SB_TYPE_DATE: {
            int32_t days = scratchbird::core::TypeExtractor::ymdToDays(
                value->data.date_val.year,
                value->data.date_val.month,
                value->data.date_val.day);
            int32_t days_since_2000 = days - kDaysFrom1970To2000;
            out.data.resize(4);
            out.data[0] = static_cast<uint8_t>(days_since_2000 & 0xFF);
            out.data[1] = static_cast<uint8_t>((days_since_2000 >> 8) & 0xFF);
            out.data[2] = static_cast<uint8_t>((days_since_2000 >> 16) & 0xFF);
            out.data[3] = static_cast<uint8_t>((days_since_2000 >> 24) & 0xFF);
            return SB_OK;
        }
        case SB_TYPE_TIME:
        case SB_TYPE_TIME_TZ: {
            int64_t micros = (static_cast<int64_t>(value->data.time_val.hour) * 3600 +
                              static_cast<int64_t>(value->data.time_val.minute) * 60 +
                              static_cast<int64_t>(value->data.time_val.second)) * kMicrosPerSecond +
                             value->data.time_val.microsecond;
            out.data.resize(8);
            for (size_t i = 0; i < 8; ++i) {
                out.data[i] = static_cast<uint8_t>((static_cast<uint64_t>(micros) >> (8 * i)) & 0xFF);
            }
            return SB_OK;
        }
        case SB_TYPE_INTERVAL: {
            out.data.resize(16);
            std::memcpy(out.data.data(), &value->data.interval_val.micros, sizeof(int64_t));
            std::memcpy(out.data.data() + 8, &value->data.interval_val.days, sizeof(int32_t));
            std::memcpy(out.data.data() + 12, &value->data.interval_val.months, sizeof(int32_t));
            return SB_OK;
        }
        case SB_TYPE_TIMESTAMP:
        case SB_TYPE_TIMESTAMP_TZ: {
            int64_t micros_since_2000 = value->data.timestamp_val.epoch_microseconds - kMicrosFrom1970To2000;
            out.data.resize(8);
            for (size_t i = 0; i < 8; ++i) {
                out.data[i] = static_cast<uint8_t>((static_cast<uint64_t>(micros_since_2000) >> (8 * i)) & 0xFF);
            }
            return SB_OK;
        }
        case SB_TYPE_UUID:
            out.data.assign(value->data.uuid_val.bytes,
                            value->data.uuid_val.bytes + 16);
            return SB_OK;
        default:
            return SB_ERR_INVALID_PARAM;
    }
}

} // namespace

sb_connection* sb_connect(const char* conn_str, sb_error* err) {
    if (!conn_str) {
        set_error(err, SB_ERR_NULL_POINTER, "Connection string is required");
        return nullptr;
    }
    auto* conn = new sb_connection();
    conn->connection_id = "conn-" + std::to_string(kConnectionCounter.fetch_add(1) + 1);
    scratchbird::core::ErrorContext ctx;
    scratchbird::client::applyDriverDefaultsFromEnv(conn->config);
    auto status = scratchbird::client::parseDriverConnectionString(conn_str, conn->config, &ctx);
    if (status != scratchbird::core::Status::OK) {
        set_error(err, map_status(status), ctx.message);
        delete conn;
        return nullptr;
    }
    status = conn->client.connect(conn->config, &ctx);
    if (status != scratchbird::core::Status::OK) {
        set_error(err, map_status(status), ctx.message.empty() ? conn->client.lastError() : ctx.message);
        delete conn;
        return nullptr;
    }
    conn->keepalive_manager.Start();
    conn->keepalive_tracker = conn->keepalive_manager.Register(conn->connection_id, conn);
    conn->leak_detector.Start();
    conn->leak_guard = conn->leak_detector.Checkout(conn->connection_id);
    set_error(err, SB_OK, "");
    return conn;
}

void sb_disconnect(sb_connection* conn) {
    if (!conn) {
        return;
    }
    if (conn->keepalive_tracker) {
        conn->keepalive_manager.Unregister(conn->connection_id);
        conn->keepalive_tracker = nullptr;
    }
    conn->keepalive_manager.Stop();
    conn->leak_guard.reset();
    conn->leak_detector.Stop();
    conn->client.disconnect();
    delete conn;
}

char* sb_probe_auth_surface_json(const char* conn_str, sb_error* err) {
    if (!conn_str) {
        set_error(err, SB_ERR_NULL_POINTER, "Connection string is required");
        return nullptr;
    }
    scratchbird::client::AuthProbeResult result;
    scratchbird::core::ErrorContext ctx;
    auto status = scratchbird::client::probeAuthSurface(conn_str, &result, &ctx);
    if (status != scratchbird::core::Status::OK) {
        set_error(err, map_status(status), ctx.message);
        return nullptr;
    }
    return allocateCString(authProbeResultToJson(result),
                           err,
                           "Out of memory allocating auth probe JSON");
}

char* sb_get_resolved_auth_context_json(sb_connection* conn, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return nullptr;
    }
    return allocateCString(resolvedAuthContextToJson(conn->client.getResolvedAuthContext()),
                           err,
                           "Out of memory allocating resolved auth context JSON");
}

sb_result* sb_execute(sb_connection* conn, const char* sql, sb_error* err) {
    if (!conn || !sql) {
        set_error(err, SB_ERR_NULL_POINTER, "Connection and SQL required");
        return nullptr;
    }
    OperationSpan span;
    if (!start_operation(conn, "execute", sql, span, err)) {
        return nullptr;
    }
    auto* result = new sb_result();
    scratchbird::core::ErrorContext ctx;
    auto status = conn->client.executeQuery(sql, result->results, &ctx);
    if (status != scratchbird::core::Status::OK) {
        set_error(err, map_status(status), ctx.message.empty() ? conn->client.lastError() : ctx.message);
        end_operation(conn, span, false);
        delete result;
        return nullptr;
    }
    result->column_names.reserve(result->results.columns.size());
    for (const auto& col : result->results.columns) {
        result->column_names.push_back(col.name);
    }
    set_error(err, SB_OK, "");
    end_operation(conn, span, true);
    return result;
}

sb_result* sb_query(sb_connection* conn, const char* sql, sb_error* err) {
    return sb_execute(conn, sql, err);
}

sb_result* sb_metadata_query(sb_connection* conn, const char* collection_name, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_NULL_POINTER, "Connection required");
        return nullptr;
    }

    const std::string requested = collection_name ? collection_name : std::string();
    std::string metadata_sql;
    std::string normalized_collection;
    if (!scratchbird::client::resolveMetadataCollectionQuery(
            requested,
            &metadata_sql,
            &normalized_collection)) {
        set_error(
            err,
            SB_ERR_NOT_IMPLEMENTED,
            scratchbird::client::metadataCollectionNotSupportedMessage(requested));
        return nullptr;
    }

    return sb_query(conn, metadata_sql.c_str(), err);
}

char* sb_metadata_schema_payload(sb_connection* conn,
                                 const char* schema_pattern,
                                 int expand_schema_parents,
                                 sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_NULL_POINTER, "Connection required");
        return nullptr;
    }

    sb_error query_err{};
    sb_result* schemas = sb_metadata_query(conn, "schemas", &query_err);
    if (!schemas) {
        set_error(err, query_err.code, query_err.message);
        return nullptr;
    }

    const int schema_col = findSchemaNameColumnIndex(schemas);
    if (schema_col < 0) {
        sb_result_free(schemas);
        set_error(err, SB_ERR_PROTOCOL, "schemas metadata result is missing schema-name column");
        return nullptr;
    }

    std::vector<std::string> schema_names;
    schema_names.reserve(schemas->results.rows.size());
    for (const auto& row : schemas->results.rows) {
        if (static_cast<size_t>(schema_col) >= row.size()) {
            continue;
        }
        const std::string schema_name = decodeTextColumnValue(row[static_cast<size_t>(schema_col)]);
        if (!schema_name.empty()) {
            schema_names.push_back(schema_name);
        }
    }
    sb_result_free(schemas);

    const std::string pattern_value = schema_pattern ? std::string(schema_pattern) : std::string();
    const std::string* pattern_ptr = schema_pattern ? &pattern_value : nullptr;
    const std::string payload_json = scratchbird::client::buildMetadataDdlEditorSchemaPayloadJson(
        schema_names,
        pattern_ptr,
        expand_schema_parents != 0);

    return allocateCString(payload_json,
                           err,
                           "Out of memory allocating metadata payload",
                           "metadata_schema_payload_json");
}

int sb_fetch(sb_result* result, sb_row* row, sb_error* err) {
    if (!result || !row) {
        set_error(err, SB_ERR_NULL_POINTER, "Result and row required");
        return SB_ERR_NULL_POINTER;
    }
    if (result->row_index >= result->results.rows.size()) {
        set_error(err, SB_ERR_RESULT_EXHAUSTED, "No more rows");
        return SB_ERR_RESULT_EXHAUSTED;
    }
    row->result = result;
    row->row_index = result->row_index++;
    set_error(err, SB_OK, "");
    return SB_OK;
}

void sb_result_free(sb_result* result) {
    delete result;
}

int64_t sb_rows_affected(sb_result* result) {
    if (!result) {
        return 0;
    }
    return result->results.rows_affected;
}

int sb_memory_describe(const void* ptr, sb_memory_ownership_info* out, sb_error* err) {
    if (!out) {
        set_error(err, SB_ERR_NULL_POINTER, "Memory ownership output is required");
        return SB_ERR_NULL_POINTER;
    }
    std::memset(out, 0, sizeof(*out));
    out->abi_version = 1;
    copyAbiText(out->authority_scope, sizeof(out->authority_scope), kAbiMemoryAuthorityScope);
    if (!ptr) {
        out->ownership_kind = SB_MEMORY_OWNERSHIP_UNKNOWN;
        set_error(err, SB_ERR_NULL_POINTER, "Pointer is null");
        return SB_ERR_NULL_POINTER;
    }

    AbiAllocationRecord record;
    {
        std::lock_guard<std::mutex> lock(abiAllocationMutex());
        auto it = abiAllocations().find(const_cast<void*>(ptr));
        if (it == abiAllocations().end()) {
            out->ownership_kind = SB_MEMORY_OWNERSHIP_UNKNOWN;
            copyAbiText(out->purpose, sizeof(out->purpose), "unknown_or_borrowed_pointer");
            set_error(err, SB_ERR_INVALID_PARAM, "Pointer is not a driver-owned ABI allocation");
            return SB_ERR_INVALID_PARAM;
        }
        record = it->second;
    }

    out->ownership_kind = SB_MEMORY_OWNERSHIP_DRIVER_ALLOCATED;
    out->bytes = record.bytes;
    copyAbiText(out->purpose, sizeof(out->purpose), record.purpose);
    copyAbiText(out->release_function, sizeof(out->release_function), "sb_memory_release");
    set_error(err, SB_OK, "");
    return SB_OK;
}

int sb_memory_release(void* ptr, sb_error* err) {
    if (!ptr) {
        set_error(err, SB_OK, "");
        return SB_OK;
    }
    AbiAllocationRecord record;
    if (!untrackAbiAllocation(ptr, &record)) {
        set_error(err, SB_ERR_INVALID_PARAM, "Pointer is not a driver-owned ABI allocation");
        return SB_ERR_INVALID_PARAM;
    }
    std::free(ptr);
    set_error(err, SB_OK, "");
    return SB_OK;
}

void sb_memory_free(void* ptr) {
    sb_error ignored{};
    (void)sb_memory_release(ptr, &ignored);
}

int sb_column_count(sb_result* result) {
    if (!result) {
        return 0;
    }
    return static_cast<int>(result->results.columns.size());
}

int sb_get_column_meta(sb_result* result, int index, sb_column_meta* out) {
    if (!result || !out) {
        return SB_ERR_NULL_POINTER;
    }
    if (index < 0 || static_cast<size_t>(index) >= result->results.columns.size()) {
        return SB_ERR_INVALID_PARAM;
    }
    const auto& col = result->results.columns[static_cast<size_t>(index)];
    out->name = result->column_names[static_cast<size_t>(index)].c_str();
    out->type = map_type_oid(col.type_oid);
    out->type_modifier = static_cast<int32_t>(col.type_modifier);
    out->nullable = col.nullable ? 1 : 0;
    return SB_OK;
}

int sb_value_get(sb_row* row, int column, sb_value* out) {
    if (!row || !row->result || !out) {
        return SB_ERR_NULL_POINTER;
    }
    const auto& results = row->result->results;
    if (row->row_index >= results.rows.size()) {
        return SB_ERR_RESULT_EXHAUSTED;
    }
    if (column < 0 || static_cast<size_t>(column) >= results.rows[row->row_index].size()) {
        return SB_ERR_INVALID_PARAM;
    }
    const auto& value = results.rows[row->row_index][static_cast<size_t>(column)];
    uint32_t type_oid = 0;
    if (static_cast<size_t>(column) < results.columns.size()) {
        type_oid = results.columns[static_cast<size_t>(column)].type_oid;
    }
    out->type = map_type_oid(type_oid);
    out->type_oid = type_oid;
    out->is_null = value.is_null ? 1 : 0;
    if (value.is_null) {
        return SB_OK;
    }
    const auto& data = value.data;
    switch (out->type) {
        case SB_TYPE_BOOLEAN:
            out->data.boolean_val = (!data.empty() && data[0]) ? 1 : 0;
            break;
        case SB_TYPE_SMALLINT: {
            int16_t v = 0;
            if (data.size() >= sizeof(v)) {
                std::memcpy(&v, data.data(), sizeof(v));
            }
            out->data.smallint_val = v;
            break;
        }
        case SB_TYPE_INTEGER: {
            int32_t v = 0;
            if (data.size() >= sizeof(v)) {
                std::memcpy(&v, data.data(), sizeof(v));
            }
            out->data.integer_val = v;
            break;
        }
        case SB_TYPE_BIGINT: {
            int64_t v = 0;
            if (data.size() >= sizeof(v)) {
                std::memcpy(&v, data.data(), sizeof(v));
            }
            out->data.bigint_val = v;
            break;
        }
        case SB_TYPE_REAL: {
            float v = 0;
            if (data.size() >= sizeof(v)) {
                std::memcpy(&v, data.data(), sizeof(v));
            }
            out->data.real_val = v;
            break;
        }
        case SB_TYPE_DOUBLE: {
            double v = 0;
            if (data.size() >= sizeof(v)) {
                std::memcpy(&v, data.data(), sizeof(v));
            }
            out->data.double_val = v;
            break;
        }
        case SB_TYPE_BLOB:
            out->data.binary_val.data = data.data();
            out->data.binary_val.length = data.size();
            break;
        case SB_TYPE_MONEY: {
            int64_t cents = 0;
            if (data.size() >= sizeof(cents)) {
                std::memcpy(&cents, data.data(), sizeof(cents));
            }
            out->data.money_val = cents;
            break;
        }
        case SB_TYPE_DATE: {
            int32_t days = 0;
            if (data.size() >= sizeof(days)) {
                std::memcpy(&days, data.data(), sizeof(days));
            }
            decode_date(days, out);
            break;
        }
        case SB_TYPE_TIME:
        case SB_TYPE_TIME_TZ: {
            int64_t micros = 0;
            if (data.size() >= sizeof(micros)) {
                std::memcpy(&micros, data.data(), sizeof(micros));
            }
            decode_time(micros, out);
            break;
        }
        case SB_TYPE_INTERVAL: {
            int64_t micros = 0;
            int32_t days = 0;
            int32_t months = 0;
            if (data.size() >= 16) {
                std::memcpy(&micros, data.data(), sizeof(micros));
                std::memcpy(&days, data.data() + 8, sizeof(days));
                std::memcpy(&months, data.data() + 12, sizeof(months));
            }
            out->data.interval_val.micros = micros;
            out->data.interval_val.days = days;
            out->data.interval_val.months = months;
            break;
        }
        case SB_TYPE_TIMESTAMP:
        case SB_TYPE_TIMESTAMP_TZ: {
            int64_t micros = 0;
            if (data.size() >= sizeof(micros)) {
                std::memcpy(&micros, data.data(), sizeof(micros));
            }
            decode_timestamp(micros + kMicrosFrom1970To2000, out);
            break;
        }
        case SB_TYPE_UUID: {
            std::memset(out->data.uuid_val.bytes, 0, sizeof(out->data.uuid_val.bytes));
            if (data.size() >= 16) {
                std::memcpy(out->data.uuid_val.bytes, data.data(), 16);
            }
            break;
        }
        case SB_TYPE_GEOMETRY:
        case SB_TYPE_ARRAY:
        case SB_TYPE_VECTOR:
        case SB_TYPE_INET:
        case SB_TYPE_CIDR:
        case SB_TYPE_MACADDR:
            out->data.binary_val.data = data.data();
            out->data.binary_val.length = data.size();
            break;
        case SB_TYPE_COMPOSITE:
        case SB_TYPE_RANGE:
        case SB_TYPE_UNKNOWN:
            out->data.binary_val.data = data.data();
            out->data.binary_val.length = data.size();
            break;
        case SB_TYPE_CHAR:
        case SB_TYPE_VARCHAR:
        case SB_TYPE_TEXT:
        case SB_TYPE_JSON:
        case SB_TYPE_JSONB:
        case SB_TYPE_XML:
        case SB_TYPE_TSVECTOR:
        case SB_TYPE_TSQUERY:
        case SB_TYPE_DECIMAL: {
            out->data.string_val.data = data.empty() ? "" : reinterpret_cast<const char*>(data.data());
            out->data.string_val.length = data.size();
            break;
        }
        default:
            if (out->type == SB_TYPE_BLOB) {
                out->data.binary_val.data = data.data();
                out->data.binary_val.length = data.size();
            } else {
                out->data.string_val.data = data.empty() ? "" : reinterpret_cast<const char*>(data.data());
                out->data.string_val.length = data.size();
            }
            break;
    }
    return SB_OK;
}

int sb_get_int64(sb_row* row, int column, int64_t* out) {
    if (!out) {
        return SB_ERR_NULL_POINTER;
    }
    sb_value value{};
    auto status = sb_value_get(row, column, &value);
    if (status != SB_OK) {
        return status;
    }
    if (value.is_null) {
        *out = 0;
        return SB_OK;
    }
    switch (value.type) {
        case SB_TYPE_SMALLINT:
            *out = value.data.smallint_val;
            return SB_OK;
        case SB_TYPE_INTEGER:
            *out = value.data.integer_val;
            return SB_OK;
        case SB_TYPE_BIGINT:
            *out = value.data.bigint_val;
            return SB_OK;
        default:
            return SB_ERR_TYPE_MISMATCH;
    }
}

const char* sb_get_string(sb_row* row, int column, size_t* length) {
    sb_value value{};
    if (sb_value_get(row, column, &value) != SB_OK) {
        if (length) {
            *length = 0;
        }
        return nullptr;
    }
    if (value.is_null) {
        if (length) {
            *length = 0;
        }
        return nullptr;
    }
    switch (value.type) {
        case SB_TYPE_BLOB:
        case SB_TYPE_GEOMETRY:
        case SB_TYPE_ARRAY:
        case SB_TYPE_COMPOSITE:
        case SB_TYPE_RANGE:
        case SB_TYPE_VECTOR:
        case SB_TYPE_INET:
        case SB_TYPE_CIDR:
        case SB_TYPE_MACADDR:
        case SB_TYPE_UNKNOWN:
            if (length) {
                *length = value.data.binary_val.length;
            }
            return reinterpret_cast<const char*>(value.data.binary_val.data);
        default:
            break;
    }
    if (value.data.string_val.data == nullptr) {
        if (length) {
            *length = 0;
        }
        return nullptr;
    }
    if (length) {
        *length = value.data.string_val.length;
    }
    return value.data.string_val.data;
}

sb_prepared* sb_prepare(sb_connection* conn, const char* sql, sb_error* err) {
    if (!conn || !sql) {
        set_error(err, SB_ERR_NULL_POINTER, "Connection and SQL required");
        return nullptr;
    }
    auto* stmt = new sb_prepared();
    stmt->conn = conn;
    scratchbird::core::ErrorContext ctx;
    auto status = conn->client.prepare(sql, stmt->stmt, &ctx);
    if (status != scratchbird::core::Status::OK) {
        set_error(err, map_status(status), ctx.message);
        delete stmt;
        return nullptr;
    }
    parse_named_params(sql, stmt->param_names);
    set_error(err, SB_OK, "");
    return stmt;
}

int sb_bind_index(sb_prepared* stmt, size_t index, const sb_value* value, sb_error* err) {
    if (!stmt) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Statement is null");
        return SB_ERR_INVALID_HANDLE;
    }
    auto code = apply_bind_value(stmt->stmt, index, value);
    if (code != SB_OK) {
        set_error(err, static_cast<sb_error_code>(code), "Failed to bind parameter");
    } else {
        set_error(err, SB_OK, "");
    }
    return code;
}

int sb_bind_name(sb_prepared* stmt, const char* name, const sb_value* value, sb_error* err) {
    if (!stmt || !name) {
        set_error(err, SB_ERR_NULL_POINTER, "Name required");
        return SB_ERR_NULL_POINTER;
    }
    for (size_t i = 0; i < stmt->param_names.size(); ++i) {
        if (stmt->param_names[i] == name) {
            return sb_bind_index(stmt, i + 1, value, err);
        }
    }
    set_error(err, SB_ERR_INVALID_PARAM, "Parameter name not found");
    return SB_ERR_INVALID_PARAM;
}

sb_result* sb_execute_prepared(sb_prepared* stmt, sb_error* err) {
    if (!stmt || !stmt->conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Statement is null");
        return nullptr;
    }
    OperationSpan span;
    if (!start_operation(stmt->conn, "execute_prepared", nullptr, span, err)) {
        return nullptr;
    }
    auto* result = new sb_result();
    scratchbird::core::ErrorContext ctx;
    auto status = stmt->conn->client.executePrepared(stmt->stmt, result->results, &ctx);
    if (status != scratchbird::core::Status::OK) {
        set_error(err, map_status(status), ctx.message);
        end_operation(stmt->conn, span, false);
        delete result;
        return nullptr;
    }
    result->column_names.reserve(result->results.columns.size());
    for (const auto& col : result->results.columns) {
        result->column_names.push_back(col.name);
    }
    set_error(err, SB_OK, "");
    end_operation(stmt->conn, span, true);
    return result;
}

void sb_prepared_free(sb_prepared* stmt) {
    delete stmt;
}

int sb_tx_begin_ex(sb_connection* conn, const sb_txn_options* options, sb_error* err);

int sb_tx_begin(sb_connection* conn, sb_error* err) {
    const sb_txn_options defaults{};
    return sb_tx_begin_ex(conn, &defaults, err);
}

int sb_tx_begin_ex(sb_connection* conn, const sb_txn_options* options, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return SB_ERR_INVALID_HANDLE;
    }
    const sb_txn_options opts = options ? *options : sb_txn_options{};
    scratchbird::client::NetworkClient::TransactionOptions tx_opts;
    tx_opts.flags = opts.flags;
    tx_opts.conflict_action = opts.conflict_action;
    tx_opts.autocommit_mode = opts.autocommit_mode;
    tx_opts.isolation_level = opts.isolation_level;
    tx_opts.read_committed_mode = opts.read_committed_mode;
    tx_opts.access_mode = opts.access_mode;
    tx_opts.deferrable = opts.deferrable;
    tx_opts.wait_mode = opts.wait_mode;
    tx_opts.timeout_ms = opts.timeout_ms;
    scratchbird::core::ErrorContext ctx;
    auto status = conn->client.beginTransaction(tx_opts, &ctx);
    set_error(err, map_status(status), ctx.message);
    return status == scratchbird::core::Status::OK ? SB_OK : map_status(status);
}

int sb_tx_commit(sb_connection* conn, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return SB_ERR_INVALID_HANDLE;
    }
    scratchbird::core::ErrorContext ctx;
    auto status = conn->client.commit(&ctx);
    set_error(err, map_status(status), ctx.message);
    return status == scratchbird::core::Status::OK ? SB_OK : map_status(status);
}

int sb_tx_rollback(sb_connection* conn, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return SB_ERR_INVALID_HANDLE;
    }
    scratchbird::core::ErrorContext ctx;
    auto status = conn->client.rollback(&ctx);
    set_error(err, map_status(status), ctx.message);
    return status == scratchbird::core::Status::OK ? SB_OK : map_status(status);
}

int sb_tx_prepare_transaction(sb_connection* conn,
                              const char* global_transaction_id,
                              sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return SB_ERR_INVALID_HANDLE;
    }
    std::string sql;
    if (!buildPreparedTransactionSql("PREPARE TRANSACTION", global_transaction_id, sql, err)) {
        return err ? static_cast<int>(err->code) : SB_ERR_UNKNOWN;
    }
    return executeTransactionControlSql(conn, sql, err);
}

int sb_tx_commit_prepared(sb_connection* conn,
                          const char* global_transaction_id,
                          sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return SB_ERR_INVALID_HANDLE;
    }
    std::string sql;
    if (!buildPreparedTransactionSql("COMMIT PREPARED", global_transaction_id, sql, err)) {
        return err ? static_cast<int>(err->code) : SB_ERR_UNKNOWN;
    }
    return executeTransactionControlSql(conn, sql, err);
}

int sb_tx_rollback_prepared(sb_connection* conn,
                            const char* global_transaction_id,
                            sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return SB_ERR_INVALID_HANDLE;
    }
    std::string sql;
    if (!buildPreparedTransactionSql("ROLLBACK PREPARED", global_transaction_id, sql, err)) {
        return err ? static_cast<int>(err->code) : SB_ERR_UNKNOWN;
    }
    return executeTransactionControlSql(conn, sql, err);
}

int sb_tx_detach_to_dormant(sb_connection* conn, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return SB_ERR_INVALID_HANDLE;
    }
    set_error(err,
              SB_ERR_NOT_IMPLEMENTED,
              "dormant detach/reattach is not exposed by the public C/C++ front door");
    return SB_ERR_NOT_IMPLEMENTED;
}

int sb_tx_reattach_dormant(sb_connection* conn,
                           const char* dormant_id,
                           const char* auth_token,
                           sb_error* err) {
    (void)dormant_id;
    (void)auth_token;
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return SB_ERR_INVALID_HANDLE;
    }
    set_error(err,
              SB_ERR_NOT_IMPLEMENTED,
              "dormant detach/reattach is not exposed by the public C/C++ front door");
    return SB_ERR_NOT_IMPLEMENTED;
}

int sb_tx_savepoint(sb_connection* conn, const char* name, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return SB_ERR_INVALID_HANDLE;
    }
    if (!name || name[0] == '\0') {
        set_error(err, SB_ERR_INVALID_PARAM, "Savepoint name is required");
        return SB_ERR_INVALID_PARAM;
    }
    scratchbird::core::ErrorContext ctx;
    auto status = conn->client.savepoint(name, &ctx);
    set_error(err, map_status(status), ctx.message);
    return status == scratchbird::core::Status::OK ? SB_OK : map_status(status);
}

int sb_tx_release_savepoint(sb_connection* conn, const char* name, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return SB_ERR_INVALID_HANDLE;
    }
    if (!name || name[0] == '\0') {
        set_error(err, SB_ERR_INVALID_PARAM, "Savepoint name is required");
        return SB_ERR_INVALID_PARAM;
    }
    scratchbird::core::ErrorContext ctx;
    auto status = conn->client.releaseSavepoint(name, &ctx);
    set_error(err, map_status(status), ctx.message);
    return status == scratchbird::core::Status::OK ? SB_OK : map_status(status);
}

int sb_tx_rollback_to(sb_connection* conn, const char* name, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return SB_ERR_INVALID_HANDLE;
    }
    if (!name || name[0] == '\0') {
        set_error(err, SB_ERR_INVALID_PARAM, "Savepoint name is required");
        return SB_ERR_INVALID_PARAM;
    }
    scratchbird::core::ErrorContext ctx;
    auto status = conn->client.rollbackToSavepoint(name, &ctx);
    set_error(err, map_status(status), ctx.message);
    return status == scratchbird::core::Status::OK ? SB_OK : map_status(status);
}

int sb_cancel(sb_connection* conn, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return SB_ERR_INVALID_HANDLE;
    }
    scratchbird::core::ErrorContext ctx;
    auto status = conn->client.sendQueryCancel(&ctx);
    set_error(err, map_status(status), ctx.message);
    return status == scratchbird::core::Status::OK ? SB_OK : map_status(status);
}

int sb_set_option(sb_connection* conn, const char* name, const char* value, sb_error* err) {
    if (!conn || !name || !value) {
        set_error(err, SB_ERR_NULL_POINTER, "Connection, name, and value required");
        return SB_ERR_NULL_POINTER;
    }
    scratchbird::core::ErrorContext ctx;
    auto status = conn->client.setOption(name, value, &ctx);
    set_error(err, map_status(status), ctx.message);
    return status == scratchbird::core::Status::OK ? SB_OK : map_status(status);
}

int sb_ping(sb_connection* conn, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return SB_ERR_INVALID_HANDLE;
    }
    scratchbird::core::ErrorContext ctx;
    auto status = conn->client.ping(&ctx);
    set_error(err, map_status(status), ctx.message);
    if (status == scratchbird::core::Status::OK) {
        drain_notifications(conn);
    }
    return status == scratchbird::core::Status::OK ? SB_OK : map_status(status);
}

int sb_is_healthy(sb_connection* conn, sb_error* err) {
    return sb_ping(conn, err) == SB_OK ? 1 : 0;
}

int sb_subscribe(sb_connection* conn, uint8_t subscribe_type,
                 const char* channel, const char* filter, sb_error* err) {
    if (!conn || !channel) {
        set_error(err, SB_ERR_NULL_POINTER, "Connection and channel required");
        return SB_ERR_NULL_POINTER;
    }
    scratchbird::core::ErrorContext ctx;
    auto status = conn->client.subscribeNotifications(
        subscribe_type,
        channel,
        filter ? filter : "",
        &ctx);
    set_error(err, map_status(status), ctx.message);
    if (status == scratchbird::core::Status::OK) {
        drain_notifications(conn);
    }
    return status == scratchbird::core::Status::OK ? SB_OK : map_status(status);
}

int sb_unsubscribe(sb_connection* conn, const char* channel, sb_error* err) {
    if (!conn || !channel) {
        set_error(err, SB_ERR_NULL_POINTER, "Connection and channel required");
        return SB_ERR_NULL_POINTER;
    }
    scratchbird::core::ErrorContext ctx;
    auto status = conn->client.unsubscribeNotifications(channel, &ctx);
    set_error(err, map_status(status), ctx.message);
    if (status == scratchbird::core::Status::OK) {
        drain_notifications(conn);
    }
    return status == scratchbird::core::Status::OK ? SB_OK : map_status(status);
}

int sb_listen(sb_connection* conn, const char* channel, const char* filter, sb_error* err) {
    return sb_subscribe(conn, scratchbird::protocol::kSubTypeChannel, channel, filter, err);
}

int sb_unlisten(sb_connection* conn, const char* channel, sb_error* err) {
    return sb_unsubscribe(conn, channel, err);
}

int sb_unlisten_all(sb_connection* conn, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return SB_ERR_INVALID_HANDLE;
    }
    sb_result* result = sb_execute(conn, "UNLISTEN *", err);
    if (!result) {
        return err ? err->code : SB_ERR_UNKNOWN;
    }
    sb_result_free(result);
    set_error(err, SB_OK, "");
    return SB_OK;
}

int sb_notify_channel(sb_connection* conn,
                      const char* channel,
                      const uint8_t* payload,
                      size_t payload_len,
                      sb_error* err) {
    if (!conn || !channel) {
        set_error(err, SB_ERR_NULL_POINTER, "Connection and channel required");
        return SB_ERR_NULL_POINTER;
    }
    std::string sql = "NOTIFY " + quoteSqlIdentifier(channel);
    if (payload && payload_len > 0) {
        const std::string payload_text = "hex:" + hexEncode(payload, payload_len);
        sql += ", " + quoteSqlLiteral(payload_text);
    }
    sb_result* result = sb_execute(conn, sql.c_str(), err);
    if (!result) {
        return err ? err->code : SB_ERR_UNKNOWN;
    }
    sb_result_free(result);
    set_error(err, SB_OK, "");
    return SB_OK;
}

int sb_poll_notifications(sb_connection* conn, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return SB_ERR_INVALID_HANDLE;
    }
    const int count = drain_notifications(conn);
    set_error(err, SB_OK, "");
    return count;
}

size_t sb_notification_count(sb_connection* conn) {
    if (!conn) {
        return 0;
    }
    drain_notifications(conn);
    std::lock_guard<std::mutex> lock(conn->notification_mutex);
    return conn->notification_queue.size();
}

char* sb_get_notification(sb_connection* conn, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return nullptr;
    }
    drain_notifications(conn);
    scratchbird::client::NetworkClient::Notification notice;
    {
        std::lock_guard<std::mutex> lock(conn->notification_mutex);
        if (conn->notification_queue.empty()) {
            set_error(err, SB_OK, "");
            return nullptr;
        }
        notice = conn->notification_queue.front();
        conn->notification_queue.pop_front();
    }
    std::ostringstream payload;
    payload << "{"
            << "\"process_id\":" << notice.process_id << ","
            << "\"channel\":\"" << jsonEscape(notice.channel) << "\","
            << "\"payload_hex\":\"" << hexEncode(notice.payload) << "\","
            << "\"change_type\":" << static_cast<uint32_t>(notice.change_type) << ","
            << "\"row_id\":" << notice.row_id << ","
            << "\"has_row_id\":" << (notice.row_id != 0 ? "true" : "false")
            << "}";
    return allocateCString(payload.str(), err, "Out of memory allocating notification payload");
}

char* sb_get_notifications(sb_connection* conn, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return nullptr;
    }
    drain_notifications(conn);
    std::deque<scratchbird::client::NetworkClient::Notification> notices;
    {
        std::lock_guard<std::mutex> lock(conn->notification_mutex);
        notices.swap(conn->notification_queue);
    }
    std::ostringstream payload;
    payload << "[";
    for (size_t i = 0; i < notices.size(); ++i) {
        const auto& notice = notices[i];
        if (i != 0) {
            payload << ",";
        }
        payload << "{"
                << "\"process_id\":" << notice.process_id << ","
                << "\"channel\":\"" << jsonEscape(notice.channel) << "\","
                << "\"payload_hex\":\"" << hexEncode(notice.payload) << "\","
                << "\"change_type\":" << static_cast<uint32_t>(notice.change_type) << ","
                << "\"row_id\":" << notice.row_id << ","
                << "\"has_row_id\":" << (notice.row_id != 0 ? "true" : "false")
                << "}";
    }
    payload << "]";
    return allocateCString(payload.str(), err, "Out of memory allocating notifications payload");
}

void sb_clear_notifications(sb_connection* conn) {
    if (!conn) {
        return;
    }
    std::lock_guard<std::mutex> lock(conn->notification_mutex);
    conn->notification_queue.clear();
}

uint64_t sb_add_notification_listener(sb_connection* conn,
                                      sb_notification_listener_fn listener,
                                      void* user_data,
                                      sb_error* err) {
    if (!conn || !listener) {
        set_error(err, SB_ERR_NULL_POINTER, "Connection and listener required");
        return 0;
    }
    std::lock_guard<std::mutex> lock(conn->notification_mutex);
    const uint64_t id = conn->next_listener_id++;
    conn->notification_listeners[id] = sb_connection::NotificationListener{listener, user_data};
    set_error(err, SB_OK, "");
    return id;
}

int sb_remove_notification_listener(sb_connection* conn,
                                    uint64_t listener_id,
                                    sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return SB_ERR_INVALID_HANDLE;
    }
    std::lock_guard<std::mutex> lock(conn->notification_mutex);
    auto it = conn->notification_listeners.find(listener_id);
    if (it == conn->notification_listeners.end()) {
        set_error(err, SB_ERR_INVALID_PARAM, "Notification listener not found");
        return SB_ERR_INVALID_PARAM;
    }
    conn->notification_listeners.erase(it);
    set_error(err, SB_OK, "");
    return SB_OK;
}

int sb_stream_control(sb_connection* conn, uint8_t control_type,
                      uint32_t window_size, uint32_t timeout_ms, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return SB_ERR_INVALID_HANDLE;
    }
    scratchbird::core::ErrorContext ctx;
    auto status = conn->client.streamControl(control_type, window_size, timeout_ms, &ctx);
    set_error(err, map_status(status), ctx.message);
    return status == scratchbird::core::Status::OK ? SB_OK : map_status(status);
}

char* sb_get_telemetry_summary_json(sb_connection* conn, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return nullptr;
    }
    const auto metrics = conn->telemetry.GetMetrics();
    std::ostringstream json;
    json << "{"
         << "\"total_invocations\":" << metrics.total_queries << ","
         << "\"total_successes\":" << metrics.successful_queries << ","
         << "\"total_failures\":" << metrics.failed_queries << ","
         << "\"total_duration_ms\":" << metrics.total_query_time_ms << ","
         << "\"operations\":[";
    size_t index = 0;
    for (const auto& pair : metrics.operation_metrics) {
        if (index++ != 0) {
            json << ",";
        }
        json << "{"
             << "\"operation\":\"" << jsonEscape(pair.first) << "\","
             << "\"invocations\":" << pair.second.count << ","
             << "\"failures\":" << pair.second.error_count << ","
             << "\"successes\":" << (pair.second.count - pair.second.error_count) << ","
             << "\"total_duration_ms\":" << pair.second.total_time_ms << ","
             << "\"average_duration_ms\":" << pair.second.avg_time_ms << ","
             << "\"max_duration_ms\":" << pair.second.max_time_ms
             << "}";
    }
    json << "],"
         << "\"histogram\":{"
         << "\"ms_0_10\":" << metrics.latency_histogram.ms_0_10 << ","
         << "\"ms_10_100\":" << metrics.latency_histogram.ms_10_100 << ","
         << "\"ms_100_1000\":" << metrics.latency_histogram.ms_100_1000 << ","
         << "\"ms_1000_10000\":" << metrics.latency_histogram.ms_1000_10000 << ","
         << "\"ms_over_10000\":" << metrics.latency_histogram.ms_over_10000
         << "}"
         << "}";
    return allocateCString(json.str(), err, "Out of memory allocating telemetry summary");
}

int sb_reset_telemetry(sb_connection* conn, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return SB_ERR_INVALID_HANDLE;
    }
    conn->telemetry.Reset();
    set_error(err, SB_OK, "");
    return SB_OK;
}

char* sb_get_slow_operations_json(sb_connection* conn, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return nullptr;
    }
    return allocateCString(conn->telemetry.ExportSlowQueriesJson(),
                           err,
                           "Out of memory allocating slow operation summary");
}

char* sb_export_telemetry_prometheus(sb_connection* conn, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return nullptr;
    }
    return allocateCString(conn->telemetry.ExportPrometheusMetrics(),
                           err,
                           "Out of memory allocating telemetry export");
}

char* sb_get_circuit_breaker_summary_json(sb_connection* conn, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return nullptr;
    }
    const auto stats = conn->circuit_breaker.GetStats();
    const char* state = "open";
    switch (stats.state) {
        case scratchbird::sb_circuit_state::SB_CIRCUIT_CLOSED:
            state = "closed";
            break;
        case scratchbird::sb_circuit_state::SB_CIRCUIT_HALF_OPEN:
            state = "half_open";
            break;
        case scratchbird::sb_circuit_state::SB_CIRCUIT_OPEN:
        default:
            state = "open";
            break;
    }
    std::ostringstream json;
    json << "{"
         << "\"state\":\"" << state << "\","
         << "\"failure_count\":" << stats.failure_count << ","
         << "\"success_count\":" << stats.success_count << ","
         << "\"half_open_requests\":" << stats.half_open_requests
         << "}";
    return allocateCString(json.str(), err, "Out of memory allocating circuit summary");
}

char* sb_get_keepalive_summary_json(sb_connection* conn, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return nullptr;
    }
    const auto monitored = conn->keepalive_manager.GetMonitoredCount();
    uint64_t idle_ms = 0;
    if (conn->keepalive_tracker) {
        idle_ms = static_cast<uint64_t>(conn->keepalive_tracker->GetIdleDuration().count());
    }
    std::ostringstream json;
    json << "{"
         << "\"monitored_count\":" << monitored << ","
         << "\"idle_duration_ms\":" << idle_ms
         << "}";
    return allocateCString(json.str(), err, "Out of memory allocating keepalive summary");
}

char* sb_get_leak_summary_json(sb_connection* conn, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return nullptr;
    }
    return allocateCString(conn->leak_detector.ExportDiagnosticsJson(),
                           err,
                           "Out of memory allocating checkout leak summary");
}

char* sb_get_diagnostics_json(sb_connection* conn, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return nullptr;
    }
    drain_notifications(conn);
    const auto metrics = conn->telemetry.GetMetrics();
    const auto circuit = conn->circuit_breaker.GetStats();
    const auto leak = conn->leak_detector.GetStats();
    const auto monitored = conn->keepalive_manager.GetMonitoredCount();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const char* circuit_state = "open";
    switch (circuit.state) {
        case scratchbird::sb_circuit_state::SB_CIRCUIT_CLOSED:
            circuit_state = "closed";
            break;
        case scratchbird::sb_circuit_state::SB_CIRCUIT_HALF_OPEN:
            circuit_state = "half_open";
            break;
        case scratchbird::sb_circuit_state::SB_CIRCUIT_OPEN:
        default:
            circuit_state = "open";
            break;
    }
    size_t queue_depth = 0;
    {
        std::lock_guard<std::mutex> lock(conn->notification_mutex);
        queue_depth = conn->notification_queue.size();
    }
    std::ostringstream json;
    json << "{"
         << "\"captured_unix_ms\":" << now_ms << ","
         << "\"is_healthy\":" << (conn->client.isConnected() ? "true" : "false") << ","
         << "\"front_door_mode\":\"" << jsonEscape(conn->config.front_door_mode) << "\","
         << "\"protocol\":\"" << jsonEscape(conn->config.protocol) << "\","
         << "\"host\":\"" << jsonEscape(conn->config.host) << "\","
         << "\"port\":" << conn->config.port << ","
         << "\"database\":\"" << jsonEscape(conn->config.database) << "\","
         << "\"telemetry_total_invocations\":" << metrics.total_queries << ","
         << "\"telemetry_total_failures\":" << metrics.failed_queries << ","
         << "\"circuit_state\":\"" << circuit_state << "\","
         << "\"keepalive_monitored_count\":" << monitored << ","
         << "\"checkout_leak_detector_kind\":\""
         << scratchbird::client::LeakDetector::DetectorKind() << "\","
         << "\"checkout_leak_authority_scope\":\""
         << scratchbird::client::LeakDetector::AuthorityScope() << "\","
         << "\"leak_active_checkouts\":" << leak.active_checkouts << ","
         << "\"leak_potential_count\":" << leak.potential_leaks << ","
         << "\"notification_queue_depth\":" << queue_depth
         << "}";
    return allocateCString(json.str(), err, "Out of memory allocating diagnostics payload");
}

int sb_attach_create(sb_connection* conn, const char* mode, const char* db_name, sb_error* err) {
    if (!conn || !mode || !db_name) {
        set_error(err, SB_ERR_NULL_POINTER, "Connection, mode, and database required");
        return SB_ERR_NULL_POINTER;
    }
    scratchbird::core::ErrorContext ctx;
    auto status = conn->client.attachCreate(mode, db_name, &ctx);
    set_error(err, map_status(status), ctx.message);
    return status == scratchbird::core::Status::OK ? SB_OK : map_status(status);
}

int sb_attach_detach(sb_connection* conn, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return SB_ERR_INVALID_HANDLE;
    }
    scratchbird::core::ErrorContext ctx;
    auto status = conn->client.attachDetach(&ctx);
    set_error(err, map_status(status), ctx.message);
    return status == scratchbird::core::Status::OK ? SB_OK : map_status(status);
}

sb_result* sb_attach_list(sb_connection* conn, sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return nullptr;
    }
    auto* result = new sb_result();
    scratchbird::core::ErrorContext ctx;
    auto status = conn->client.attachList(result->results, &ctx);
    if (status != scratchbird::core::Status::OK) {
        set_error(err, map_status(status), ctx.message.empty() ? conn->client.lastError() : ctx.message);
        delete result;
        return nullptr;
    }
    result->column_names.reserve(result->results.columns.size());
    for (const auto& col : result->results.columns) {
        result->column_names.push_back(col.name);
    }
    set_error(err, SB_OK, "");
    return result;
}

sb_result* sb_execute_sblr(sb_connection* conn,
                           uint64_t sblr_hash,
                           const uint8_t* bytecode,
                           size_t bytecode_len,
                           const sb_value* params,
                           size_t param_count,
                           sb_error* err) {
    if (!conn) {
        set_error(err, SB_ERR_INVALID_HANDLE, "Connection is null");
        return nullptr;
    }
    if (!bytecode && bytecode_len > 0) {
        set_error(err, SB_ERR_NULL_POINTER, "Bytecode required");
        return nullptr;
    }
    if (param_count > 0 && !params) {
        set_error(err, SB_ERR_NULL_POINTER, "Parameters required");
        return nullptr;
    }
    std::vector<uint8_t> bytecode_vec;
    if (bytecode && bytecode_len > 0) {
        bytecode_vec.assign(bytecode, bytecode + bytecode_len);
    }
    std::vector<scratchbird::protocol::ParamValue> param_values;
    param_values.reserve(param_count);
    for (size_t i = 0; i < param_count; ++i) {
        scratchbird::protocol::ParamValue param;
        int code = build_param_value(&params[i], param);
        if (code != SB_OK) {
            set_error(err, static_cast<sb_error_code>(code), "Failed to bind parameter");
            return nullptr;
        }
        param_values.push_back(std::move(param));
    }
    auto* result = new sb_result();
    scratchbird::core::ErrorContext ctx;
    auto status = conn->client.executeSblr(sblr_hash, bytecode_vec, param_values, result->results, &ctx);
    if (status != scratchbird::core::Status::OK) {
        set_error(err, map_status(status), ctx.message.empty() ? conn->client.lastError() : ctx.message);
        delete result;
        return nullptr;
    }
    result->column_names.reserve(result->results.columns.size());
    for (const auto& col : result->results.columns) {
        result->column_names.push_back(col.name);
    }
    set_error(err, SB_OK, "");
    return result;
}

#ifdef SCRATCHBIRD_TEST_HOOKS
sb_type sb_test_map_type_oid(uint32_t oid) {
    return map_type_oid(oid);
}

uint32_t sb_test_map_sb_type_to_oid(sb_type type) {
    return map_sb_type_to_oid(type);
}

char* sb_test_allocate_owned_memory(const char* payload, sb_error* err) {
    return allocateCString(payload ? std::string(payload) : std::string(),
                           err,
                           "Out of memory allocating test-owned ABI memory",
                           "test_owned_memory");
}

size_t sb_test_driver_owned_allocation_count(void) {
    return trackedAbiAllocationCount();
}
#endif
