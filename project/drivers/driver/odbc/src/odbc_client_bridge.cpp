// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/odbc/odbc_client_bridge.h"

#include <cstring>
#include <iomanip>
#include <sstream>
#include <cctype>

#include "scratchbird/core/type_extractor.h"
#include "scratchbird/client/driver_config.h"

namespace scratchbird {
namespace odbc {

namespace {
SQLRETURN statusToReturn(core::Status status) {
    return status == core::Status::OK ? SQL_SUCCESS : SQL_ERROR;
}

constexpr int32_t kDaysFrom1970To2000 = 10957;
constexpr int64_t kMicrosPerSecond = 1000000LL;
constexpr int64_t kMicrosPerDay = 86400LL * kMicrosPerSecond;

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

std::vector<std::string> splitMethodList(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token = trimAscii(token);
        if (!token.empty()) {
            out.push_back(token);
        }
    }
    return out;
}

template <typename T>
bool decodeScalar(const std::vector<uint8_t>& data, T& out) {
    if (data.size() < sizeof(T)) {
        return false;
    }
    std::memcpy(&out, data.data(), sizeof(T));
    return true;
}

std::string formatDateFromEpochDays(int64_t days_since_epoch) {
    int32_t year = core::TypeExtractor::extractYear(days_since_epoch);
    int32_t month = core::TypeExtractor::extractMonth(days_since_epoch);
    int32_t day = core::TypeExtractor::extractDay(days_since_epoch);

    std::ostringstream ss;
    ss << std::setw(4) << std::setfill('0') << year << '-'
       << std::setw(2) << std::setfill('0') << month << '-'
       << std::setw(2) << std::setfill('0') << day;
    return ss.str();
}

std::string formatTimeFromMicros(int64_t micros) {
    int32_t hour = core::TypeExtractor::extractHour(micros);
    int32_t minute = core::TypeExtractor::extractMinute(micros);
    int32_t second = core::TypeExtractor::extractSecond(micros);
    int32_t micro = core::TypeExtractor::extractMicrosecond(micros);

    std::ostringstream ss;
    ss << std::setw(2) << std::setfill('0') << hour << ':'
       << std::setw(2) << std::setfill('0') << minute << ':'
       << std::setw(2) << std::setfill('0') << second;
    if (micro != 0) {
        ss << '.' << std::setw(6) << std::setfill('0') << micro;
    }
    return ss.str();
}

std::string formatTimestampFromMicros(int64_t micros) {
    int32_t year = core::TypeExtractor::extractTimestampYear(micros);
    int32_t month = core::TypeExtractor::extractTimestampMonth(micros);
    int32_t day = core::TypeExtractor::extractTimestampDay(micros);
    int32_t hour = core::TypeExtractor::extractTimestampHour(micros);
    int32_t minute = core::TypeExtractor::extractTimestampMinute(micros);
    int32_t second = core::TypeExtractor::extractTimestampSecond(micros);
    int32_t micro = core::TypeExtractor::extractTimestampMicrosecond(micros);

    std::ostringstream ss;
    ss << std::setw(4) << std::setfill('0') << year << '-'
       << std::setw(2) << std::setfill('0') << month << '-'
       << std::setw(2) << std::setfill('0') << day << ' '
       << std::setw(2) << std::setfill('0') << hour << ':'
       << std::setw(2) << std::setfill('0') << minute << ':'
       << std::setw(2) << std::setfill('0') << second;
    if (micro != 0) {
        ss << '.' << std::setw(6) << std::setfill('0') << micro;
    }
    return ss.str();
}

std::string formatTimestampWithOffset(int64_t micros, int16_t offset_minutes) {
    int64_t local_micros = micros + static_cast<int64_t>(offset_minutes) * 60 * kMicrosPerSecond;
    std::string base = formatTimestampFromMicros(local_micros);

    int16_t abs_offset = static_cast<int16_t>(offset_minutes < 0 ? -offset_minutes : offset_minutes);
    int16_t offset_hours = abs_offset / 60;
    int16_t offset_mins = abs_offset % 60;

    std::ostringstream ss;
    ss << base << (offset_minutes < 0 ? '-' : '+')
       << std::setw(2) << std::setfill('0') << offset_hours << ':'
       << std::setw(2) << std::setfill('0') << offset_mins;
    return ss.str();
}

std::string formatInterval(int32_t months, int32_t days, int64_t micros) {
    int64_t total_seconds = micros / kMicrosPerSecond;
    int64_t micro = micros % kMicrosPerSecond;
    int64_t hours = total_seconds / 3600;
    int64_t minutes = (total_seconds / 60) % 60;
    int64_t seconds = total_seconds % 60;

    std::ostringstream ss;
    ss << months << " months " << days << " days "
       << std::setw(2) << std::setfill('0') << hours << ':'
       << std::setw(2) << std::setfill('0') << minutes << ':'
       << std::setw(2) << std::setfill('0') << seconds;
    if (micro != 0) {
        int64_t abs_micro = micro < 0 ? -micro : micro;
        ss << '.' << std::setw(6) << std::setfill('0') << abs_micro;
    }
    return ss.str();
}

std::string formatUuid(const std::vector<uint8_t>& data) {
    if (data.size() < 16) {
        return "";
    }
    std::ostringstream ss;
    ss << std::hex << std::nouppercase << std::setfill('0');
    for (size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            ss << '-';
        }
        ss << std::setw(2) << static_cast<int>(data[i]);
    }
    return ss.str();
}

} // namespace

OdbcClientBridge::OdbcClientBridge() = default;
OdbcClientBridge::~OdbcClientBridge() = default;

SQLRETURN OdbcClientBridge::connect(const ConnectionParams& params, std::string& error) {
    auto config = buildConfig(params);
    core::ErrorContext ctx;
    auto status = client_.connect(config, &ctx);
    last_status_ = status;
    resolved_auth_context_ = client_.getResolvedAuthContext();
    if (status != core::Status::OK) {
        last_error_ = ctx.message.empty() ? client_.lastError() : ctx.message;
        error = last_error_;
    } else {
        last_error_.clear();
    }
    return statusToReturn(status);
}

SQLRETURN OdbcClientBridge::probeAuthSurface(const ConnectionParams& params,
                                             client::AuthProbeResult& result,
                                             std::string& error) {
    auto config = buildConfig(params);
    core::ErrorContext ctx;
    auto status = client_.probeAuthSurface(config, result, &ctx);
    last_status_ = status;
    resolved_auth_context_ = client_.getResolvedAuthContext();
    if (status != core::Status::OK) {
        last_error_ = ctx.message.empty() ? client_.lastError() : ctx.message;
        error = last_error_;
    } else {
        last_error_.clear();
        error.clear();
    }
    return statusToReturn(status);
}

void OdbcClientBridge::disconnect() {
    client_.disconnect();
    resolved_auth_context_ = client_.getResolvedAuthContext();
}

bool OdbcClientBridge::isConnected() const {
    return client_.isConnected();
}

client::ResolvedAuthContext OdbcClientBridge::getResolvedAuthContext() const {
    return resolved_auth_context_;
}

SQLRETURN OdbcClientBridge::executeSQL(const std::string& sql,
                                       std::vector<std::vector<std::string>>& results,
                                       std::vector<ColumnMetadata>& columns,
                                       SQLLEN& rows_affected) {
    results.clear();
    columns.clear();
    rows_affected = 0;

    client::NetworkResultSet net_results;
    core::ErrorContext ctx;
    auto status = client_.executeQuery(sql, net_results, &ctx);
    last_status_ = status;
    if (status != core::Status::OK) {
        last_error_ = ctx.message.empty() ? client_.lastError() : ctx.message;
        return statusToReturn(status);
    }
    last_error_.clear();

    columns.reserve(net_results.columns.size());
    for (const auto& col : net_results.columns) {
        columns.push_back(mapColumn(col));
    }

    results.reserve(net_results.rows.size());
    for (const auto& row : net_results.rows) {
        std::vector<std::string> out_row;
        out_row.reserve(row.size());
        for (size_t i = 0; i < row.size(); ++i) {
            uint32_t type_oid = 0;
            if (i < net_results.columns.size()) {
                type_oid = net_results.columns[i].type_oid;
            }
            out_row.push_back(stringifyValue(row[i], type_oid));
        }
        results.push_back(std::move(out_row));
    }

    rows_affected = static_cast<SQLLEN>(net_results.rows_affected);
    return SQL_SUCCESS;
}

SQLRETURN OdbcClientBridge::cancel() {
    core::ErrorContext ctx;
    auto status = client_.sendQueryCancel(&ctx);
    last_status_ = status;
    if (status != core::Status::OK) {
        last_error_ = ctx.message.empty() ? client_.lastError() : ctx.message;
        return statusToReturn(status);
    }
    last_error_.clear();
    return SQL_SUCCESS;
}

SQLRETURN OdbcClientBridge::beginTransaction() {
    auto status = client_.beginTransaction();
    last_status_ = status;
    last_error_ = status == core::Status::OK ? "" : client_.lastError();
    return statusToReturn(status);
}

SQLRETURN OdbcClientBridge::commit() {
    auto status = client_.commit();
    last_status_ = status;
    last_error_ = status == core::Status::OK ? "" : client_.lastError();
    return statusToReturn(status);
}

SQLRETURN OdbcClientBridge::rollback() {
    auto status = client_.rollback();
    last_status_ = status;
    last_error_ = status == core::Status::OK ? "" : client_.lastError();
    return statusToReturn(status);
}

client::NetworkClientConfig OdbcClientBridge::buildConfig(const ConnectionParams& params) {
    constexpr const char* kDefaultSessionSchema = "users.public";
    client::NetworkClientConfig cfg;
    cfg.host = params.server.empty() ? "127.0.0.1" : params.server;
    cfg.port = params.port;
    cfg.database = params.database;
    cfg.username = params.user;
    cfg.password = params.password;
    cfg.schema = params.schema.empty() ? kDefaultSessionSchema : params.schema;
    cfg.protocol = params.protocol.empty() ? "native" : params.protocol;
    cfg.application_name = params.application_name.empty() ? "scratchbird_odbc" : params.application_name;
    cfg.connect_timeout_ms = params.connect_timeout * 1000;
    if (params.query_timeout > 0) {
        cfg.read_timeout_ms = params.query_timeout * 1000;
        cfg.write_timeout_ms = params.query_timeout * 1000;
    }
    cfg.ssl_mode = client::parseSslMode(params.ssl_mode);
    cfg.ssl_cert = params.ssl_cert;
    cfg.ssl_key = params.ssl_key;
    cfg.ssl_root_cert = params.ssl_root_cert;
    cfg.front_door_mode = params.front_door_mode;
    cfg.manager_auth_token = params.manager_auth_token;
    cfg.manager_username = params.manager_username;
    cfg.manager_database = params.manager_database;
    cfg.manager_connection_profile = params.manager_connection_profile;
    cfg.manager_client_intent = params.manager_client_intent;
    cfg.manager_client_flags = params.manager_client_flags;
    cfg.manager_auth_fast_path = params.manager_auth_fast_path;
    cfg.connect_client_flags = params.connect_client_flags;
    cfg.auth_method_id = params.auth_method_id;
    cfg.auth_token = params.auth_token;
    cfg.auth_method_payload = params.auth_method_payload;
    cfg.auth_payload_json = params.auth_payload_json;
    cfg.auth_payload_b64 = params.auth_payload_b64;
    cfg.auth_provider_profile = params.auth_provider_profile;
    cfg.auth_required_methods = splitMethodList(params.auth_required_methods);
    cfg.auth_forbidden_methods = splitMethodList(params.auth_forbidden_methods);
    cfg.auth_require_channel_binding = params.auth_require_channel_binding;
    cfg.workload_identity_token = params.workload_identity_token;
    cfg.proxy_principal_assertion = params.proxy_principal_assertion;
    client::applyDriverDefaultsFromEnv(cfg);
    return cfg;
}

ColumnMetadata OdbcClientBridge::mapColumn(const client::NetworkColumn& col) {
    ColumnMetadata meta;
    meta.name = col.name;
    meta.type_name = typeOidToString(col.type_oid);
    meta.sql_type = mapTypeOid(col.type_oid);
    return meta;
}

SQLSMALLINT OdbcClientBridge::mapTypeOid(uint32_t type_oid) {
    using namespace scratchbird::protocol;
    switch (type_oid) {
        case kOidBool:
            return SQL_BIT;
        case kOidInt2:
            return SQL_SMALLINT;
        case kOidInt4:
            return SQL_INTEGER;
        case kOidInt8:
            return SQL_BIGINT;
        case kOidFloat4:
            return SQL_REAL;
        case kOidFloat8:
            return SQL_DOUBLE;
        case kOidNumeric:
        case kOidMoney:
            return SQL_DECIMAL;
        case kOidChar:
        case kOidBpChar:
            return SQL_CHAR;
        case kOidVarchar:
        case kOidText:
        case kOidJson:
        case kOidJsonb:
        case kOidXml:
        case kOidTsVector:
        case kOidTsQuery:
        case kOidRecord:
        case kOidInt4Range:
        case kOidNumRange:
        case kOidTsRange:
        case kOidTstzRange:
        case kOidDateRange:
        case kOidInt8Range:
            return SQL_LONGVARCHAR;
        case kOidBytea:
        case kOidPoint:
        case kOidLseg:
        case kOidPath:
        case kOidBox:
        case kOidPolygon:
        case kOidLine:
        case kOidCircle:
        case kOidSbVector:
            return SQL_LONGVARBINARY;
        case kOidDate:
            return SQL_TYPE_DATE;
        case kOidTime:
            return SQL_TYPE_TIME;
        case kOidTimestamp:
        case kOidTimestamptz:
            return SQL_TYPE_TIMESTAMP;
        case kOidInterval:
            return SQL_VARCHAR;
        case kOidUuid:
            return SQL_GUID;
        case kOidInet:
        case kOidCidr:
        case kOidMacaddr:
        case kOidMacaddr8:
            return SQL_VARCHAR;
        default:
            return SQL_VARCHAR;
    }
}

std::string OdbcClientBridge::typeOidToString(uint32_t type_oid) {
    using namespace scratchbird::protocol;
    switch (type_oid) {
        case kOidBool: return "BOOLEAN";
        case kOidInt2: return "SMALLINT";
        case kOidInt4: return "INTEGER";
        case kOidInt8: return "BIGINT";
        case kOidFloat4: return "REAL";
        case kOidFloat8: return "DOUBLE";
        case kOidNumeric: return "DECIMAL";
        case kOidMoney: return "MONEY";
        case kOidChar: return "CHAR";
        case kOidBpChar: return "CHAR";
        case kOidVarchar: return "VARCHAR";
        case kOidText: return "TEXT";
        case kOidJson: return "JSON";
        case kOidJsonb: return "JSONB";
        case kOidXml: return "XML";
        case kOidBytea: return "BYTEA";
        case kOidDate: return "DATE";
        case kOidTime: return "TIME";
        case kOidTimestamp: return "TIMESTAMP";
        case kOidTimestamptz: return "TIMESTAMPTZ";
        case kOidInterval: return "INTERVAL";
        case kOidUuid: return "UUID";
        case kOidInet: return "INET";
        case kOidCidr: return "CIDR";
        case kOidMacaddr: return "MACADDR";
        case kOidMacaddr8: return "MACADDR8";
        case kOidTsVector: return "TSVECTOR";
        case kOidTsQuery: return "TSQUERY";
        case kOidRecord: return "RECORD";
        case kOidInt4Range: return "INT4RANGE";
        case kOidInt8Range: return "INT8RANGE";
        case kOidNumRange: return "NUMRANGE";
        case kOidTsRange: return "TSRANGE";
        case kOidTstzRange: return "TSTZRANGE";
        case kOidDateRange: return "DATERANGE";
        case kOidSbVector: return "VECTOR";
        default: return "TEXT";
    }
}

std::string OdbcClientBridge::stringifyValue(const protocol::ColumnValue& val,
                                             uint32_t type_oid) {
    if (val.is_null) {
        return "";
    }

    auto stripLengthPrefix = [](const std::vector<uint8_t>& data, const uint8_t** out_ptr, size_t* out_len) {
        const uint8_t* ptr = data.empty() ? nullptr : data.data();
        size_t len = data.size();
        if (data.size() >= 4) {
            uint32_t payload_len = static_cast<uint32_t>(data[0]) |
                (static_cast<uint32_t>(data[1]) << 8) |
                (static_cast<uint32_t>(data[2]) << 16) |
                (static_cast<uint32_t>(data[3]) << 24);
            if (payload_len <= data.size() - 4) {
                ptr = data.data() + 4;
                len = payload_len;
            }
        }
        if (out_ptr) {
            *out_ptr = ptr;
        }
        if (out_len) {
            *out_len = len;
        }
    };

    switch (type_oid) {
        case protocol::kOidBool:
            return (!val.data.empty() && val.data[0] != 0) ? "1" : "0";
        case protocol::kOidInt2: {
            int16_t out = 0;
            if (!decodeScalar(val.data, out)) return "0";
            return std::to_string(out);
        }
        case protocol::kOidInt4: {
            int32_t out = 0;
            if (!decodeScalar(val.data, out)) return "0";
            return std::to_string(out);
        }
        case protocol::kOidInt8: {
            int64_t out = 0;
            if (!decodeScalar(val.data, out)) return "0";
            return std::to_string(out);
        }
        case protocol::kOidFloat4: {
            float out = 0.0f;
            if (!decodeScalar(val.data, out)) return "0";
            std::ostringstream ss;
            ss << out;
            return ss.str();
        }
        case protocol::kOidFloat8: {
            double out = 0.0;
            if (!decodeScalar(val.data, out)) return "0";
            std::ostringstream ss;
            ss << out;
            return ss.str();
        }
        case protocol::kOidNumeric:
        case protocol::kOidChar:
        case protocol::kOidBpChar:
        case protocol::kOidVarchar:
        case protocol::kOidText:
        case protocol::kOidJson:
        case protocol::kOidJsonb:
        case protocol::kOidXml:
        case protocol::kOidRecord:
        case protocol::kOidInet:
        case protocol::kOidCidr:
        case protocol::kOidMacaddr:
        case protocol::kOidMacaddr8:
        case protocol::kOidTsVector:
        case protocol::kOidTsQuery:
        case protocol::kOidInt4Range:
        case protocol::kOidNumRange:
        case protocol::kOidTsRange:
        case protocol::kOidTstzRange:
        case protocol::kOidDateRange:
        case protocol::kOidInt8Range: {
            const uint8_t* raw_ptr = nullptr;
            size_t raw_len = 0;
            stripLengthPrefix(val.data, &raw_ptr, &raw_len);
            return raw_ptr ? std::string(reinterpret_cast<const char*>(raw_ptr), raw_len) : std::string();
        }
        case protocol::kOidDate: {
            int32_t days32 = 0;
            int64_t days64 = 0;
            if (val.data.size() == sizeof(int32_t) && decodeScalar(val.data, days32)) {
                return formatDateFromEpochDays(static_cast<int64_t>(days32) + kDaysFrom1970To2000);
            }
            if (val.data.size() == sizeof(int64_t) && decodeScalar(val.data, days64)) {
                return formatDateFromEpochDays(days64);
            }
            return std::string(val.data.begin(), val.data.end());
        }
        case protocol::kOidTime: {
            int64_t micros = 0;
            if (decodeScalar(val.data, micros)) {
                return formatTimeFromMicros(micros);
            }
            return std::string(val.data.begin(), val.data.end());
        }
        case protocol::kOidTimestamp: {
            int64_t micros = 0;
            if (decodeScalar(val.data, micros)) {
                return formatTimestampFromMicros(micros + kDaysFrom1970To2000 * kMicrosPerDay);
            }
            return std::string(val.data.begin(), val.data.end());
        }
        case protocol::kOidTimestamptz: {
            int64_t micros = 0;
            if (decodeScalar(val.data, micros)) {
                return formatTimestampFromMicros(micros + kDaysFrom1970To2000 * kMicrosPerDay);
            }
            return std::string(val.data.begin(), val.data.end());
        }
        case protocol::kOidInterval: {
            if (val.data.size() >= sizeof(int32_t) * 2 + sizeof(int64_t)) {
                int32_t months = 0;
                int32_t days = 0;
                int64_t micros = 0;
                std::memcpy(&months, val.data.data(), sizeof(int32_t));
                std::memcpy(&days, val.data.data() + sizeof(int32_t), sizeof(int32_t));
                std::memcpy(&micros, val.data.data() + sizeof(int32_t) * 2, sizeof(int64_t));
                return formatInterval(months, days, micros);
            }
            return std::string(val.data.begin(), val.data.end());
        }
        case protocol::kOidUuid:
            if (val.data.size() == 16) {
                return formatUuid(val.data);
            }
            return std::string(val.data.begin(), val.data.end());
        case protocol::kOidMoney: {
            int64_t cents = 0;
            if (!decodeScalar(val.data, cents)) {
                return std::string(val.data.begin(), val.data.end());
            }
            bool negative = cents < 0;
            int64_t abs_cents = negative ? -cents : cents;
            int64_t units = abs_cents / 100;
            int64_t frac = abs_cents % 100;
            std::ostringstream ss;
            if (negative) {
                ss << '-';
            }
            ss << units << '.' << std::setw(2) << std::setfill('0') << frac;
            return ss.str();
        }
        case protocol::kOidBytea:
        case protocol::kOidPoint:
        case protocol::kOidLseg:
        case protocol::kOidPath:
        case protocol::kOidBox:
        case protocol::kOidPolygon:
        case protocol::kOidLine:
        case protocol::kOidCircle:
        case protocol::kOidSbVector: {
            const uint8_t* raw_ptr = nullptr;
            size_t raw_len = 0;
            stripLengthPrefix(val.data, &raw_ptr, &raw_len);
            return raw_ptr ? std::string(reinterpret_cast<const char*>(raw_ptr), raw_len) : std::string();
        }
        default:
            return std::string(val.data.begin(), val.data.end());
    }
}

} // namespace odbc
} // namespace scratchbird
