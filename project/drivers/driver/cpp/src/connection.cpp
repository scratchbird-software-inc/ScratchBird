// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/client/connection.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <sstream>

#include "scratchbird/client/driver_config.h"
#include "scratchbird/client/metadata.h"
#include "scratchbird/client/network_client.h"
#include "scratchbird/core/type_extractor.h"
#include "scratchbird/protocol/sbwp_protocol.h"

namespace scratchbird {
namespace client {

namespace {
constexpr int32_t kDaysFrom1970To2000 = 10957;
constexpr int64_t kMicrosPerSecond = 1000000LL;
constexpr int64_t kMicrosPerDay = 86400LL * kMicrosPerSecond;

bool looksLikeDriverConnectionString(const std::string& value) {
    return value.rfind("scratchbird://", 0) == 0 ||
           (value.find('=') != std::string::npos && value.find(';') != std::string::npos);
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
    for (size_t i = 0; i < data.size(); ++i) {
        ss << std::setw(2) << static_cast<int>(data[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) {
            ss << '-';
        }
    }
    return ss.str();
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

core::Status setFeatureNotSupported(core::ErrorContext* ctx, const char* message) {
    if (ctx) {
        ctx->set(core::Status::NOT_IMPLEMENTED, message, __FILE__, __LINE__, __func__);
        ctx->setSQLState(core::SQLSTATE_FEATURE_NOT_SUPPORTED);
    }
    return core::Status::NOT_IMPLEMENTED;
}

void stripLengthPrefix(const std::vector<uint8_t>& data,
                       const uint8_t** out_ptr,
                       size_t* out_len) {
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
}

std::string textColumnValue(const protocol::ColumnValue& val) {
    return val.is_null ? std::string() : std::string(val.data.begin(), val.data.end());
}

int64_t parseTextInt64(const protocol::ColumnValue& val) {
    try {
        return std::stoll(textColumnValue(val));
    } catch (...) {
        return 0;
    }
}

double parseTextDouble(const protocol::ColumnValue& val) {
    try {
        return std::stod(textColumnValue(val));
    } catch (...) {
        return 0.0;
    }
}

std::string stringifyValue(const protocol::ColumnValue& val, uint32_t type_oid, uint8_t format) {
    if (val.is_null) {
        return "";
    }
    if (format == protocol::kFormatText) {
        return textColumnValue(val);
    }

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

bool lookupColumnIndex(const std::vector<ColumnMeta>& columns,
                       const std::string& name,
                       size_t* out_index) {
    auto lower = [](const std::string& in) {
        std::string out = in;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    };
    std::string needle = lower(name);
    for (size_t i = 0; i < columns.size(); ++i) {
        if (lower(columns[i].name) == needle) {
            if (out_index) {
                *out_index = i;
            }
            return true;
        }
    }
    return false;
}

sb_type mapTypeOidToPublicType(uint32_t type_oid) {
    using namespace protocol;
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

void populatePublicConfigFromNetwork(const NetworkClientConfig& net_cfg,
                                     ConnectionConfig* config) {
    if (!config) {
        return;
    }
    config->database_name = net_cfg.database;
    config->username = net_cfg.username;
    config->password = net_cfg.password;
    config->protocol = net_cfg.protocol;
    config->role = net_cfg.role;
    config->current_schema = net_cfg.schema;
    config->application_name = net_cfg.application_name;
    config->transport_mode = net_cfg.transport_mode;
    config->host = net_cfg.host;
    config->tcp_port = net_cfg.port;
    config->ipc_method = net_cfg.ipc_method;
    config->ipc_path = net_cfg.ipc_path;
    config->front_door_mode = net_cfg.front_door_mode;
    config->manager_auth_token = net_cfg.manager_auth_token;
    config->manager_username = net_cfg.manager_username;
    config->manager_database = net_cfg.manager_database;
    config->manager_connection_profile = net_cfg.manager_connection_profile;
    config->manager_client_intent = net_cfg.manager_client_intent;
    config->manager_client_flags = net_cfg.manager_client_flags;
    config->manager_auth_fast_path = net_cfg.manager_auth_fast_path;
    config->connect_client_flags = net_cfg.connect_client_flags;
    config->auth_method_id = net_cfg.auth_method_id;
    config->auth_token = net_cfg.auth_token;
    config->auth_method_payload = net_cfg.auth_method_payload;
    config->auth_payload_json = net_cfg.auth_payload_json;
    config->auth_payload_b64 = net_cfg.auth_payload_b64;
    config->auth_provider_profile = net_cfg.auth_provider_profile;
    config->auth_required_methods = net_cfg.auth_required_methods;
    config->auth_forbidden_methods = net_cfg.auth_forbidden_methods;
    config->auth_require_channel_binding = net_cfg.auth_require_channel_binding;
    config->workload_identity_token = net_cfg.workload_identity_token;
    config->proxy_principal_assertion = net_cfg.proxy_principal_assertion;
    config->connect_timeout_ms = net_cfg.connect_timeout_ms;
    config->read_timeout_ms = net_cfg.read_timeout_ms;
    config->write_timeout_ms = net_cfg.write_timeout_ms;
    config->copy_window_bytes = net_cfg.copy_window_bytes;
    config->copy_chunk_bytes = net_cfg.copy_chunk_bytes;
    config->enable_copy_streaming = net_cfg.enable_copy_streaming;
    switch (net_cfg.ssl_mode) {
        case network::SSLMode::DISABLED:
            config->ssl_mode = "disable";
            break;
        case network::SSLMode::ALLOW:
            config->ssl_mode = "allow";
            break;
        case network::SSLMode::PREFER:
            config->ssl_mode = "prefer";
            break;
        case network::SSLMode::VERIFY_CA:
            config->ssl_mode = "verify-ca";
            break;
        case network::SSLMode::VERIFY_FULL:
            config->ssl_mode = "verify-full";
            break;
        case network::SSLMode::REQUIRE:
        default:
            config->ssl_mode = "require";
            break;
    }
    config->ssl_cert = net_cfg.ssl_cert;
    config->ssl_key = net_cfg.ssl_key;
    config->ssl_root_cert = net_cfg.ssl_root_cert;
    config->binary_transfer = net_cfg.binary_transfer;
    config->enable_compression = net_cfg.enable_compression;
    config->auto_commit = net_cfg.autocommit;
}

} // namespace

struct ResultSetImpl {
    std::shared_ptr<NetworkResultSet> results;
    std::vector<ColumnMeta> columns;
    int64_t row_index{-1};
};

struct ConnectionImpl {
    NetworkClient client;
    ConnectionConfig config;
    ConnectionState state{ConnectionState::DISCONNECTED};
    bool in_transaction{false};
    std::string last_error;
};

struct PreparedStatementImpl {
    ConnectionImpl* owner{nullptr};
    NetworkPreparedStatement statement;
};

void populateResultSet(ResultSetImpl* impl,
                       const std::shared_ptr<NetworkResultSet>& shared) {
    if (!impl) {
        return;
    }
    impl->results = shared;
    impl->row_index = -1;
    impl->columns.clear();
    for (size_t i = 0; i < shared->columns.size(); ++i) {
        const auto& col = shared->columns[i];
        impl->columns.push_back(
            ColumnMeta{col.name,
                       mapTypeOidToPublicType(col.type_oid),
                       col.type_oid,
                       col.type_modifier,
                       i,
                       col.format,
                       col.nullable});
    }
}

core::Status parseConnectionConfig(const std::string& conn_str,
                                   ConnectionConfig* config,
                                   core::ErrorContext* ctx) {
    if (!config) {
        if (ctx) {
            ctx->message = "config is required";
        }
        return core::Status::INVALID_ARGUMENT;
    }

    NetworkClientConfig net_cfg;
    core::Status status = parseDriverConnectionString(conn_str, net_cfg, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    populatePublicConfigFromNetwork(net_cfg, config);
    return core::Status::OK;
}

core::Status probeAuthSurface(const std::string& conn_str,
                              AuthProbeResult* result,
                              core::ErrorContext* ctx) {
    if (!result) {
        if (ctx) {
            ctx->message = "result is required";
        }
        return core::Status::INVALID_ARGUMENT;
    }

    NetworkClientConfig net_cfg;
    core::Status status = parseDriverConnectionString(conn_str, net_cfg, ctx);
    if (status != core::Status::OK) {
        return status;
    }

    NetworkClient client;
    return client.probeAuthSurface(net_cfg, *result, ctx);
}

void markConnectedTransactionState(ConnectionImpl* impl) {
    if (!impl) {
        return;
    }
    if (!impl->client.isConnected()) {
        impl->in_transaction = false;
        impl->state = ConnectionState::DISCONNECTED;
        return;
    }
    impl->in_transaction = impl->client.inTransaction();
    impl->state = impl->in_transaction
        ? ConnectionState::IN_TRANSACTION
        : ConnectionState::CONNECTED;
}

ResultSet::ResultSet() : impl_(std::make_unique<ResultSetImpl>()) {}
ResultSet::~ResultSet() = default;
ResultSet::ResultSet(ResultSet&& other) noexcept = default;
ResultSet& ResultSet::operator=(ResultSet&& other) noexcept = default;

PreparedStatement::PreparedStatement()
    : impl_(std::make_unique<PreparedStatementImpl>()) {}
PreparedStatement::~PreparedStatement() = default;
PreparedStatement::PreparedStatement(PreparedStatement&& other) noexcept = default;
PreparedStatement& PreparedStatement::operator=(PreparedStatement&& other) noexcept = default;

size_t ResultSet::getColumnCount() const {
    return impl_ && impl_->results ? impl_->columns.size() : 0;
}

std::string ResultSet::getColumnName(size_t index) const {
    if (!impl_ || index >= impl_->columns.size()) {
        return "";
    }
    return impl_->columns[index].name;
}

int ResultSet::getColumnIndex(const std::string& name) const {
    if (!impl_) {
        return -1;
    }
    size_t idx = 0;
    return lookupColumnIndex(impl_->columns, name, &idx) ? static_cast<int>(idx) : -1;
}

sb_type ResultSet::getColumnType(size_t index) const {
    if (!impl_ || index >= impl_->columns.size()) {
        return SB_TYPE_UNKNOWN;
    }
    return impl_->columns[index].type;
}

uint32_t ResultSet::getColumnTypeOid(size_t index) const {
    if (!impl_ || index >= impl_->columns.size()) {
        return 0;
    }
    return impl_->columns[index].type_oid;
}

uint8_t ResultSet::getColumnFormat(size_t index) const {
    if (!impl_ || index >= impl_->columns.size()) {
        return 0;
    }
    return impl_->columns[index].format;
}

bool ResultSet::isColumnNullable(size_t index) const {
    if (!impl_ || index >= impl_->columns.size()) {
        return true;
    }
    return impl_->columns[index].nullable;
}

const std::vector<ColumnMeta>& ResultSet::getColumns() const {
    static const std::vector<ColumnMeta> kEmpty;
    if (!impl_) {
        return kEmpty;
    }
    return impl_->columns;
}

int64_t ResultSet::getRowCount() const {
    return impl_ && impl_->results ? static_cast<int64_t>(impl_->results->rows.size()) : -1;
}

int64_t ResultSet::getRowsAffected() const {
    return impl_ && impl_->results ? impl_->results->rows_affected : -1;
}

bool ResultSet::isEmpty() const {
    return !impl_ || !impl_->results || impl_->results->rows.empty();
}

const std::string& ResultSet::getCommandTag() const {
    static const std::string kEmpty;
    if (!impl_ || !impl_->results) {
        return kEmpty;
    }
    return impl_->results->command_tag;
}

bool ResultSet::next() {
    if (!impl_ || !impl_->results) {
        return false;
    }
    if (impl_->row_index + 1 >= static_cast<int64_t>(impl_->results->rows.size())) {
        return false;
    }
    ++impl_->row_index;
    return true;
}

void ResultSet::reset() {
    if (impl_) {
        impl_->row_index = -1;
    }
}

int64_t ResultSet::getCurrentRow() const {
    return impl_ ? impl_->row_index : -1;
}

bool ResultSet::isNull(size_t column) const {
    if (!impl_ || !impl_->results || impl_->row_index < 0) {
        return true;
    }
    const auto& rows = impl_->results->rows;
    if (static_cast<size_t>(impl_->row_index) >= rows.size() || column >= rows[impl_->row_index].size()) {
        return true;
    }
    return rows[impl_->row_index][column].is_null;
}

bool ResultSet::getBool(size_t column) const {
    return getInt64(column) != 0;
}

int16_t ResultSet::getInt16(size_t column) const {
    int16_t out = 0;
    if (!impl_ || !impl_->results || impl_->row_index < 0) {
        return out;
    }
    const auto& row = impl_->results->rows[static_cast<size_t>(impl_->row_index)];
    if (column >= row.size()) {
        return out;
    }
    if (column < impl_->columns.size() && impl_->columns[column].format == protocol::kFormatText) {
        return static_cast<int16_t>(parseTextInt64(row[column]));
    }
    decodeScalar(row[column].data, out);
    return out;
}

int32_t ResultSet::getInt32(size_t column) const {
    int32_t out = 0;
    if (!impl_ || !impl_->results || impl_->row_index < 0) {
        return out;
    }
    const auto& row = impl_->results->rows[static_cast<size_t>(impl_->row_index)];
    if (column >= row.size()) {
        return out;
    }
    if (column < impl_->columns.size() && impl_->columns[column].format == protocol::kFormatText) {
        return static_cast<int32_t>(parseTextInt64(row[column]));
    }
    decodeScalar(row[column].data, out);
    return out;
}

int64_t ResultSet::getInt64(size_t column) const {
    int64_t out = 0;
    if (!impl_ || !impl_->results || impl_->row_index < 0) {
        return out;
    }
    const auto& row = impl_->results->rows[static_cast<size_t>(impl_->row_index)];
    if (column >= row.size()) {
        return out;
    }
    if (column < impl_->columns.size() && impl_->columns[column].format == protocol::kFormatText) {
        return parseTextInt64(row[column]);
    }
    decodeScalar(row[column].data, out);
    return out;
}

float ResultSet::getFloat(size_t column) const {
    float out = 0.0f;
    if (!impl_ || !impl_->results || impl_->row_index < 0) {
        return out;
    }
    const auto& row = impl_->results->rows[static_cast<size_t>(impl_->row_index)];
    if (column >= row.size()) {
        return out;
    }
    if (column < impl_->columns.size() && impl_->columns[column].format == protocol::kFormatText) {
        return static_cast<float>(parseTextDouble(row[column]));
    }
    decodeScalar(row[column].data, out);
    return out;
}

double ResultSet::getDouble(size_t column) const {
    double out = 0.0;
    if (!impl_ || !impl_->results || impl_->row_index < 0) {
        return out;
    }
    const auto& row = impl_->results->rows[static_cast<size_t>(impl_->row_index)];
    if (column >= row.size()) {
        return out;
    }
    if (column < impl_->columns.size() && impl_->columns[column].format == protocol::kFormatText) {
        return parseTextDouble(row[column]);
    }
    decodeScalar(row[column].data, out);
    return out;
}

std::string ResultSet::getString(size_t column) const {
    if (!impl_ || !impl_->results || impl_->row_index < 0) {
        return "";
    }
    const auto& row = impl_->results->rows[static_cast<size_t>(impl_->row_index)];
    if (column >= row.size() || column >= impl_->columns.size()) {
        return "";
    }
    return stringifyValue(row[column], impl_->columns[column].type_oid, impl_->columns[column].format);
}

std::vector<uint8_t> ResultSet::getBytes(size_t column) const {
    if (!impl_ || !impl_->results || impl_->row_index < 0) {
        return {};
    }
    const auto& row = impl_->results->rows[static_cast<size_t>(impl_->row_index)];
    if (column >= row.size()) {
        return {};
    }
    const uint8_t* raw_ptr = nullptr;
    size_t raw_len = 0;
    stripLengthPrefix(row[column].data, &raw_ptr, &raw_len);
    if (!raw_ptr || raw_len == 0) {
        return {};
    }
    return std::vector<uint8_t>(raw_ptr, raw_ptr + raw_len);
}

int64_t ResultSet::getTimestamp(size_t column) const {
    int64_t out = 0;
    if (!impl_ || !impl_->results || impl_->row_index < 0) {
        return out;
    }
    const auto& row = impl_->results->rows[static_cast<size_t>(impl_->row_index)];
    if (column >= row.size()) {
        return out;
    }
    decodeScalar(row[column].data, out);
    return out;
}

int32_t ResultSet::getDate(size_t column) const {
    int32_t out = 0;
    if (!impl_ || !impl_->results || impl_->row_index < 0) {
        return out;
    }
    const auto& row = impl_->results->rows[static_cast<size_t>(impl_->row_index)];
    if (column >= row.size()) {
        return out;
    }
    decodeScalar(row[column].data, out);
    return out;
}

int64_t ResultSet::getTime(size_t column) const {
    return getTimestamp(column);
}

std::string ResultSet::getUUID(size_t column) const {
    if (!impl_ || !impl_->results || impl_->row_index < 0) {
        return "";
    }
    const auto& row = impl_->results->rows[static_cast<size_t>(impl_->row_index)];
    if (column >= row.size()) {
        return "";
    }
    if (row[column].data.size() == 16) {
        return formatUuid(row[column].data);
    }
    return "";
}

const uint8_t* ResultSet::getRaw(size_t column, size_t* length) const {
    if (!impl_ || !impl_->results || impl_->row_index < 0) {
        if (length) *length = 0;
        return nullptr;
    }
    const auto& row = impl_->results->rows[static_cast<size_t>(impl_->row_index)];
    if (column >= row.size()) {
        if (length) *length = 0;
        return nullptr;
    }
    if (length) {
        *length = row[column].data.size();
    }
    return row[column].data.data();
}

bool ResultSet::isNull(const std::string& column) const {
    if (!impl_) {
        return true;
    }
    size_t idx = 0;
    return lookupColumnIndex(impl_->columns, column, &idx) ? isNull(idx) : true;
}

bool ResultSet::getBool(const std::string& column) const {
    if (!impl_) {
        return false;
    }
    size_t idx = 0;
    return lookupColumnIndex(impl_->columns, column, &idx) ? getBool(idx) : false;
}

int16_t ResultSet::getInt16(const std::string& column) const {
    if (!impl_) {
        return 0;
    }
    size_t idx = 0;
    return lookupColumnIndex(impl_->columns, column, &idx) ? getInt16(idx) : 0;
}

int32_t ResultSet::getInt32(const std::string& column) const {
    if (!impl_) {
        return 0;
    }
    size_t idx = 0;
    return lookupColumnIndex(impl_->columns, column, &idx) ? getInt32(idx) : 0;
}

int64_t ResultSet::getInt64(const std::string& column) const {
    if (!impl_) {
        return 0;
    }
    size_t idx = 0;
    return lookupColumnIndex(impl_->columns, column, &idx) ? getInt64(idx) : 0;
}

float ResultSet::getFloat(const std::string& column) const {
    if (!impl_) {
        return 0.0f;
    }
    size_t idx = 0;
    return lookupColumnIndex(impl_->columns, column, &idx) ? getFloat(idx) : 0.0f;
}

double ResultSet::getDouble(const std::string& column) const {
    if (!impl_) {
        return 0.0;
    }
    size_t idx = 0;
    return lookupColumnIndex(impl_->columns, column, &idx) ? getDouble(idx) : 0.0;
}

std::string ResultSet::getString(const std::string& column) const {
    if (!impl_) {
        return "";
    }
    size_t idx = 0;
    return lookupColumnIndex(impl_->columns, column, &idx) ? getString(idx) : "";
}

std::vector<uint8_t> ResultSet::getBytes(const std::string& column) const {
    if (!impl_) {
        return {};
    }
    size_t idx = 0;
    return lookupColumnIndex(impl_->columns, column, &idx) ? getBytes(idx) : std::vector<uint8_t>{};
}

int64_t ResultSet::getTimestamp(const std::string& column) const {
    if (!impl_) {
        return 0;
    }
    size_t idx = 0;
    return lookupColumnIndex(impl_->columns, column, &idx) ? getTimestamp(idx) : 0;
}

int32_t ResultSet::getDate(const std::string& column) const {
    if (!impl_) {
        return 0;
    }
    size_t idx = 0;
    return lookupColumnIndex(impl_->columns, column, &idx) ? getDate(idx) : 0;
}

int64_t ResultSet::getTime(const std::string& column) const {
    if (!impl_) {
        return 0;
    }
    size_t idx = 0;
    return lookupColumnIndex(impl_->columns, column, &idx) ? getTime(idx) : 0;
}

std::string ResultSet::getUUID(const std::string& column) const {
    if (!impl_) {
        return "";
    }
    size_t idx = 0;
    return lookupColumnIndex(impl_->columns, column, &idx) ? getUUID(idx) : "";
}

Connection::Connection() : impl_(std::make_unique<ConnectionImpl>()) {}
Connection::~Connection() = default;
Connection::Connection(Connection&& other) noexcept = default;
Connection& Connection::operator=(Connection&& other) noexcept = default;

core::Status Connection::connect(const std::string& database,
                                 const std::string& username,
                                 const std::string& password,
                                 core::ErrorContext* ctx) {
    NetworkClientConfig net_cfg;
    core::Status status = parseDriverConnectionString(database, net_cfg, ctx);
    if (status != core::Status::OK) {
        if (looksLikeDriverConnectionString(database)) {
            impl_->last_error = ctx ? ctx->message : std::string();
            impl_->state = ConnectionState::ERROR_STATE;
            return status;
        }
        applyDriverDefaults(net_cfg);
        net_cfg.database = database;
    }
    if (!username.empty()) {
        net_cfg.username = username;
    }
    if (!password.empty()) {
        net_cfg.password = password;
    }
    populatePublicConfigFromNetwork(net_cfg, &impl_->config);
    impl_->state = ConnectionState::CONNECTING;
    status = impl_->client.connect(net_cfg, ctx);
    impl_->last_error = impl_->client.lastError();
    if (status == core::Status::OK) {
        markConnectedTransactionState(impl_.get());
    } else {
        impl_->in_transaction = false;
        impl_->state = ConnectionState::ERROR_STATE;
    }
    return status;
}

core::Status Connection::connect(const ConnectionConfig& config,
                                 core::ErrorContext* ctx) {
    NetworkClientConfig net_cfg;
    applyDriverDefaults(net_cfg);
    net_cfg.database = config.database_name;
    net_cfg.username = config.username;
    net_cfg.password = config.password;
    net_cfg.protocol = config.protocol.empty() ? "native" : config.protocol;
    net_cfg.role = config.role;
    net_cfg.schema = config.current_schema;
    net_cfg.application_name =
        config.application_name.empty() ? "scratchbird_driver" : config.application_name;
    net_cfg.transport_mode = config.transport_mode.empty() ? "inet_listener" : config.transport_mode;
    net_cfg.host = config.host.empty() ? "127.0.0.1" : config.host;
    net_cfg.port = config.tcp_port;
    net_cfg.ipc_method = config.ipc_method.empty() ? "auto" : config.ipc_method;
    net_cfg.ipc_path = config.ipc_path;
    net_cfg.front_door_mode = config.front_door_mode.empty() ? "direct" : config.front_door_mode;
    net_cfg.manager_auth_token = config.manager_auth_token;
    net_cfg.manager_username = config.manager_username;
    net_cfg.manager_database = config.manager_database;
    net_cfg.manager_connection_profile = config.manager_connection_profile;
    net_cfg.manager_client_intent = config.manager_client_intent;
    net_cfg.manager_client_flags = config.manager_client_flags;
    net_cfg.manager_auth_fast_path = config.manager_auth_fast_path;
    net_cfg.connect_client_flags = config.connect_client_flags;
    net_cfg.auth_method_id = config.auth_method_id;
    net_cfg.auth_token = config.auth_token;
    net_cfg.auth_method_payload = config.auth_method_payload;
    net_cfg.auth_payload_json = config.auth_payload_json;
    net_cfg.auth_payload_b64 = config.auth_payload_b64;
    net_cfg.auth_provider_profile = config.auth_provider_profile;
    net_cfg.auth_required_methods = config.auth_required_methods;
    net_cfg.auth_forbidden_methods = config.auth_forbidden_methods;
    net_cfg.auth_require_channel_binding = config.auth_require_channel_binding;
    net_cfg.workload_identity_token = config.workload_identity_token;
    net_cfg.proxy_principal_assertion = config.proxy_principal_assertion;
    net_cfg.connect_timeout_ms = config.connect_timeout_ms;
    net_cfg.read_timeout_ms = config.read_timeout_ms;
    net_cfg.write_timeout_ms = config.write_timeout_ms;
    net_cfg.copy_window_bytes = config.copy_window_bytes;
    net_cfg.copy_chunk_bytes = config.copy_chunk_bytes;
    net_cfg.enable_copy_streaming = config.enable_copy_streaming;
    net_cfg.ssl_mode = parseSslMode(config.ssl_mode);
    net_cfg.ssl_cert = config.ssl_cert;
    net_cfg.ssl_key = config.ssl_key;
    net_cfg.ssl_root_cert = config.ssl_root_cert;
    net_cfg.binary_transfer = config.binary_transfer;
    net_cfg.enable_compression = config.enable_compression;
    net_cfg.autocommit = config.auto_commit;

    impl_->config = config;
    impl_->state = ConnectionState::CONNECTING;
    core::Status status = impl_->client.connect(net_cfg, ctx);
    impl_->last_error = impl_->client.lastError();
    if (status == core::Status::OK) {
        markConnectedTransactionState(impl_.get());
    } else {
        impl_->in_transaction = false;
        impl_->state = ConnectionState::ERROR_STATE;
    }
    return status;
}

core::Status Connection::attachCreate(const std::string& mode,
                                      const std::string& db_name,
                                      core::ErrorContext* ctx) {
    if (!impl_ || impl_->state != ConnectionState::CONNECTED) {
        if (ctx) {
            ctx->set(core::Status::INVALID_ARGUMENT,
                     "Connection is not open",
                     __FILE__,
                     __LINE__,
                     __func__);
        }
        return core::Status::INVALID_ARGUMENT;
    }
    auto status = impl_->client.attachCreate(mode, db_name, ctx);
    impl_->last_error = impl_->client.lastError();
    if (status != core::Status::OK) {
        impl_->state = ConnectionState::ERROR_STATE;
    }
    return status;
}

void Connection::disconnect() {
    if (!impl_) {
        return;
    }
    impl_->client.disconnect();
    impl_->state = ConnectionState::DISCONNECTED;
    impl_->in_transaction = false;
}

bool Connection::isConnected() const {
    return impl_ && impl_->client.isConnected();
}

ConnectionState Connection::getState() const {
    return impl_ ? impl_->state : ConnectionState::DISCONNECTED;
}

std::string Connection::getLastError() const {
    return impl_ ? impl_->last_error : std::string();
}

ResolvedAuthContext Connection::getResolvedAuthContext() const {
    return impl_ ? impl_->client.getResolvedAuthContext() : ResolvedAuthContext{};
}

core::Status Connection::executeQuery(const std::string& sql,
                                      ResultSet* results,
                                      core::ErrorContext* ctx) {
    return executeQuery(sql, results, 0, ctx);
}

core::Status Connection::executeQuery(const std::string& sql,
                                      ResultSet* results,
                                      uint8_t /*flags*/,
                                      core::ErrorContext* ctx) {
    auto shared = std::make_shared<NetworkResultSet>();
    core::Status status = impl_->client.executeQuery(sql, *shared, ctx);
    impl_->last_error = impl_->client.lastError();
    populateResultSet(results ? results->impl_.get() : nullptr, shared);
    if (status == core::Status::OK) {
        markConnectedTransactionState(impl_.get());
    }
    return status;
}

core::Status Connection::execute(const std::string& sql,
                                 int64_t* rows_affected,
                                 core::ErrorContext* ctx) {
    NetworkResultSet results;
    core::Status status = impl_->client.executeQuery(
        sql, results, ctx, protocol::kQueryFlagScriptSummaryResult);
    impl_->last_error = impl_->client.lastError();
    if (rows_affected) {
        *rows_affected = results.rows_affected;
    }
    if (status == core::Status::OK) {
        markConnectedTransactionState(impl_.get());
    }
    return status;
}

core::Status Connection::prepare(const std::string& sql,
                                 PreparedStatement* stmt,
                                 core::ErrorContext* ctx) {
    if (!stmt) {
        if (ctx) {
            ctx->message = "stmt is required";
        }
        impl_->last_error = "stmt is required";
        return core::Status::INVALID_ARGUMENT;
    }

    stmt->impl_->statement = NetworkPreparedStatement{};
    core::Status status = impl_->client.prepare(sql, stmt->impl_->statement, ctx);
    impl_->last_error = impl_->client.lastError();
    if (status == core::Status::OK) {
        stmt->impl_->owner = impl_.get();
        markConnectedTransactionState(impl_.get());
    } else {
        stmt->impl_->owner = nullptr;
    }
    return status;
}

core::Status Connection::metadataQuery(const std::string& collection_name,
                                       ResultSet* results,
                                       core::ErrorContext* ctx) {
    std::string query_sql;
    if (!resolveMetadataCollectionQuery(collection_name, &query_sql, nullptr)) {
        if (ctx) {
            ctx->message = metadataCollectionNotSupportedMessage(collection_name);
        }
        impl_->last_error = metadataCollectionNotSupportedMessage(collection_name);
        return core::Status::NOT_SUPPORTED;
    }
    return executeQuery(query_sql, results, ctx);
}

core::Status Connection::schemas(ResultSet* results,
                                 const std::string& schema_pattern,
                                 core::ErrorContext* ctx) {
    const std::string* pattern = schema_pattern.empty() ? nullptr : &schema_pattern;
    return executeQuery(buildMetadataSchemasQuerySql(pattern), results, ctx);
}

core::Status Connection::tables(ResultSet* results,
                                const std::string& schema_pattern,
                                const std::string& table_pattern,
                                core::ErrorContext* ctx) {
    const std::string* schema = schema_pattern.empty() ? nullptr : &schema_pattern;
    const std::string* table = table_pattern.empty() ? nullptr : &table_pattern;
    return executeQuery(buildMetadataTablesQuerySql(schema, table), results, ctx);
}

core::Status Connection::columns(ResultSet* results,
                                 const std::string& schema_pattern,
                                 const std::string& table_pattern,
                                 core::ErrorContext* ctx) {
    const std::string* schema = schema_pattern.empty() ? nullptr : &schema_pattern;
    const std::string* table = table_pattern.empty() ? nullptr : &table_pattern;
    return executeQuery(buildMetadataColumnsQuerySql(schema, table), results, ctx);
}

core::Status Connection::indexes(ResultSet* results,
                                 const std::string& schema_pattern,
                                 const std::string& table_pattern,
                                 core::ErrorContext* ctx) {
    const std::string* schema = schema_pattern.empty() ? nullptr : &schema_pattern;
    const std::string* table = table_pattern.empty() ? nullptr : &table_pattern;
    return executeQuery(buildMetadataIndexesQuerySql(schema, table), results, ctx);
}

core::Status Connection::metadataSchemaPayload(const std::string* schema_pattern,
                                               bool expand_schema_parents,
                                               std::string* payload_json,
                                               core::ErrorContext* ctx) {
    if (!payload_json) {
        if (ctx) {
            ctx->message = "payload_json is required";
        }
        impl_->last_error = "payload_json is required";
        return core::Status::INVALID_ARGUMENT;
    }

    ResultSet schemas_result;
    core::Status status = schemas(&schemas_result, "", ctx);
    if (status != core::Status::OK) {
        payload_json->clear();
        return status;
    }

    const int schema_name_index = schemas_result.getColumnIndex("schema_name");
    if (schema_name_index < 0) {
        if (ctx) {
            ctx->message = "schemas metadata result is missing schema-name column";
        }
        impl_->last_error = "schemas metadata result is missing schema-name column";
        payload_json->clear();
        return core::Status::PROTOCOL_VIOLATION;
    }

    std::vector<std::string> schema_names;
    while (schemas_result.next()) {
        schema_names.push_back(schemas_result.getString(static_cast<size_t>(schema_name_index)));
    }
    *payload_json = buildMetadataDdlEditorSchemaPayloadJson(
        schema_names,
        schema_pattern,
        expand_schema_parents);
    return core::Status::OK;
}

bool Connection::supportsPreparedTransactions() {
    return true;
}

bool Connection::supportsDormantReattach() {
    return false;
}

bool Connection::supportsPortalResume() {
    return false;
}

core::Status Connection::buildPreparedTransactionSql(const std::string& verb,
                                                     const std::string& global_transaction_id,
                                                     std::string* sql,
                                                     core::ErrorContext* ctx) {
    if (!sql) {
        if (ctx) {
            ctx->set(core::Status::INVALID_ARGUMENT,
                     "SQL output buffer is required",
                     __FILE__,
                     __LINE__,
                     __func__);
        }
        return core::Status::INVALID_ARGUMENT;
    }
    if (verb.empty()) {
        if (ctx) {
            ctx->set(core::Status::INVALID_ARGUMENT,
                     "Prepared-transaction verb is required",
                     __FILE__,
                     __LINE__,
                     __func__);
        }
        return core::Status::INVALID_ARGUMENT;
    }
    const std::string gid = trimWhitespace(global_transaction_id);
    if (gid.empty()) {
        if (ctx) {
            ctx->set(core::Status::SYNTAX_ERROR,
                     "Global transaction id is required",
                     __FILE__,
                     __LINE__,
                     __func__);
            ctx->setSQLState(core::SQLSTATE_SYNTAX_ERROR);
        }
        sql->clear();
        return core::Status::SYNTAX_ERROR;
    }
    *sql = verb + " " + quoteSqlLiteral(gid);
    return core::Status::OK;
}

core::Status Connection::beginTransaction(core::ErrorContext* ctx) {
    core::Status status = impl_->client.beginTransaction(ctx);
    impl_->last_error = impl_->client.lastError();
    if (status == core::Status::OK) {
        markConnectedTransactionState(impl_.get());
    }
    return status;
}

core::Status Connection::commit(core::ErrorContext* ctx) {
    core::Status status = impl_->client.commit(ctx);
    impl_->last_error = impl_->client.lastError();
    if (status == core::Status::OK) {
        markConnectedTransactionState(impl_.get());
    }
    return status;
}

core::Status Connection::rollback(core::ErrorContext* ctx) {
    core::Status status = impl_->client.rollback(ctx);
    impl_->last_error = impl_->client.lastError();
    if (status == core::Status::OK) {
        markConnectedTransactionState(impl_.get());
    }
    return status;
}

core::Status Connection::prepareTransaction(const std::string& global_transaction_id,
                                            core::ErrorContext* ctx) {
    std::string sql;
    core::Status status = buildPreparedTransactionSql(
        "PREPARE TRANSACTION", global_transaction_id, &sql, ctx);
    if (status != core::Status::OK) {
        impl_->last_error = ctx ? ctx->message : "Global transaction id is required";
        return status;
    }
    int64_t rows_affected = 0;
    return execute(sql, &rows_affected, ctx);
}

core::Status Connection::commitPrepared(const std::string& global_transaction_id,
                                        core::ErrorContext* ctx) {
    std::string sql;
    core::Status status = buildPreparedTransactionSql(
        "COMMIT PREPARED", global_transaction_id, &sql, ctx);
    if (status != core::Status::OK) {
        impl_->last_error = ctx ? ctx->message : "Global transaction id is required";
        return status;
    }
    int64_t rows_affected = 0;
    return execute(sql, &rows_affected, ctx);
}

core::Status Connection::rollbackPrepared(const std::string& global_transaction_id,
                                          core::ErrorContext* ctx) {
    std::string sql;
    core::Status status = buildPreparedTransactionSql(
        "ROLLBACK PREPARED", global_transaction_id, &sql, ctx);
    if (status != core::Status::OK) {
        impl_->last_error = ctx ? ctx->message : "Global transaction id is required";
        return status;
    }
    int64_t rows_affected = 0;
    return execute(sql, &rows_affected, ctx);
}

core::Status Connection::detachToDormant(core::ErrorContext* ctx) {
    const char* message =
        "dormant detach/reattach is not exposed by the public C/C++ front door";
    core::Status status = setFeatureNotSupported(ctx, message);
    impl_->last_error = message;
    return status;
}

core::Status Connection::reattachDormant(const std::string& dormant_id,
                                         const std::string& auth_token,
                                         core::ErrorContext* ctx) {
    (void)dormant_id;
    (void)auth_token;
    const char* message =
        "dormant detach/reattach is not exposed by the public C/C++ front door";
    core::Status status = setFeatureNotSupported(ctx, message);
    impl_->last_error = message;
    return status;
}

core::Status Connection::savepoint(const std::string& name,
                                   core::ErrorContext* ctx) {
    core::Status status = impl_->client.savepoint(name, ctx);
    impl_->last_error = impl_->client.lastError();
    return status;
}

core::Status Connection::releaseSavepoint(const std::string& name,
                                          core::ErrorContext* ctx) {
    core::Status status = impl_->client.releaseSavepoint(name, ctx);
    impl_->last_error = impl_->client.lastError();
    return status;
}

core::Status Connection::rollbackTo(const std::string& name,
                                    core::ErrorContext* ctx) {
    core::Status status = impl_->client.rollbackToSavepoint(name, ctx);
    impl_->last_error = impl_->client.lastError();
    return status;
}

void Connection::setAutoCommit(bool enabled) {
    if (impl_) {
        impl_->config.auto_commit = enabled;
        impl_->client.setAutocommit(enabled);
    }
}

bool Connection::getAutoCommit() const {
    return impl_ ? impl_->config.auto_commit : true;
}

bool Connection::inTransaction() const {
    return impl_ ? impl_->in_transaction : false;
}

uint64_t Connection::currentTransactionId() const {
    return impl_ ? impl_->client.currentTransactionId() : 0;
}

std::string Connection::getParameterStatus(const std::string& name) const {
    return impl_ ? impl_->client.parameterStatus(name) : std::string{};
}

const ConnectionConfig& Connection::getConfig() const {
    static const ConnectionConfig kEmpty;
    return impl_ ? impl_->config : kEmpty;
}

void Connection::setCopyInputStream(std::istream* in) {
    if (impl_) {
        impl_->client.setCopyInputStream(in);
    }
}

void Connection::setCopyInputSizeHintBytes(uint64_t bytes) {
    if (impl_) {
        impl_->client.setCopyInputSizeHintBytes(bytes);
    }
}

void Connection::setCopyPreallocationFactorPercent(uint64_t percent) {
    if (impl_) {
        impl_->client.setCopyPreallocationFactorPercent(percent);
    }
}

void Connection::setCopyOutputStream(std::ostream* out) {
    if (impl_) {
        impl_->client.setCopyOutputStream(out);
    }
}

size_t PreparedStatement::getParameterCount() const {
    return impl_ ? impl_->statement.getParameterCount() : 0;
}

bool PreparedStatement::isValid() const {
    return impl_ && impl_->owner != nullptr && impl_->statement.isValid();
}

void PreparedStatement::clearParameters() {
    if (impl_) {
        impl_->statement.clearParameters();
    }
}

void PreparedStatement::setNull(size_t index) {
    if (impl_) {
        impl_->statement.setNull(index);
    }
}

void PreparedStatement::setNull(size_t index, uint32_t type_oid) {
    if (impl_) {
        impl_->statement.setNull(index, type_oid);
    }
}

void PreparedStatement::setBool(size_t index, bool value) {
    if (impl_) {
        impl_->statement.setBool(index, value);
    }
}

void PreparedStatement::setInt16(size_t index, int16_t value) {
    if (impl_) {
        impl_->statement.setInt16(index, value);
    }
}

void PreparedStatement::setInt32(size_t index, int32_t value) {
    if (impl_) {
        impl_->statement.setInt32(index, value);
    }
}

void PreparedStatement::setInt64(size_t index, int64_t value) {
    if (impl_) {
        impl_->statement.setInt64(index, value);
    }
}

void PreparedStatement::setFloat(size_t index, float value) {
    if (impl_) {
        impl_->statement.setFloat(index, value);
    }
}

void PreparedStatement::setDouble(size_t index, double value) {
    if (impl_) {
        impl_->statement.setDouble(index, value);
    }
}

void PreparedStatement::setString(size_t index, const std::string& value) {
    if (impl_) {
        impl_->statement.setString(index, value);
    }
}

void PreparedStatement::setString(size_t index,
                                  const std::string& value,
                                  uint32_t type_oid) {
    if (impl_) {
        impl_->statement.setString(index, value, type_oid);
    }
}

void PreparedStatement::setBytes(size_t index, const std::vector<uint8_t>& value) {
    if (impl_) {
        impl_->statement.setBytes(index, value);
    }
}

void PreparedStatement::setBytes(size_t index, const uint8_t* data, size_t length) {
    if (impl_) {
        impl_->statement.setBytes(index, data, length);
    }
}

void PreparedStatement::setBinary(size_t index,
                                  const uint8_t* data,
                                  size_t length,
                                  uint32_t type_oid,
                                  bool length_prefixed) {
    if (impl_) {
        impl_->statement.setBinary(index, data, length, type_oid, length_prefixed);
    }
}

void PreparedStatement::setTimestamp(size_t index, int64_t microseconds) {
    if (impl_) {
        impl_->statement.setTimestamp(index, microseconds);
    }
}

void PreparedStatement::setDate(size_t index, int32_t days) {
    if (impl_) {
        impl_->statement.setDate(index, days);
    }
}

void PreparedStatement::setTime(size_t index, int64_t microseconds) {
    if (impl_) {
        impl_->statement.setTime(index, microseconds);
    }
}

void PreparedStatement::setUUID(size_t index, const std::vector<uint8_t>& value) {
    if (impl_) {
        impl_->statement.setUUID(index, value);
    }
}

void PreparedStatement::setUUID(size_t index, const std::string& value) {
    if (impl_) {
        impl_->statement.setUUID(index, value);
    }
}

core::Status PreparedStatement::executeQuery(ResultSet* results,
                                             core::ErrorContext* ctx) {
    if (!impl_ || impl_->owner == nullptr || !impl_->statement.isValid()) {
        if (ctx) {
            ctx->message = "prepared statement is not valid";
        }
        return core::Status::INVALID_ARGUMENT;
    }

    auto shared = std::make_shared<NetworkResultSet>();
    core::Status status =
        impl_->owner->client.executePrepared(impl_->statement, *shared, ctx);
    impl_->owner->last_error = impl_->owner->client.lastError();
    populateResultSet(results ? results->impl_.get() : nullptr, shared);
    if (status == core::Status::OK) {
        markConnectedTransactionState(impl_->owner);
    }
    return status;
}

core::Status PreparedStatement::execute(int64_t* rows_affected,
                                        core::ErrorContext* ctx) {
    ResultSet results;
    core::Status status = executeQuery(&results, ctx);
    if (rows_affected) {
        *rows_affected = (status == core::Status::OK) ? results.getRowsAffected() : 0;
    }
    return status;
}

} // namespace client
} // namespace scratchbird
