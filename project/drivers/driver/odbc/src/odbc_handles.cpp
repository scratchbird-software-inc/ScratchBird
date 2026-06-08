// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/**
 * @file odbc_handles.cpp
 * @brief ODBC Handle Implementation
 *
 * Part of Phase 3.8: ODBC Driver
 */

#include "scratchbird/odbc/odbc_handles.h"
#include "scratchbird/client/driver_config.h"
#include "scratchbird/odbc/metadata_helpers.h"
#include "scratchbird/odbc/odbc_client_bridge.h"
#include "scratchbird/core/status.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <set>

#include "scratchbird/core/type_extractor.h"

// Helper for casting pointers to integers in ODBC attributes
#define ODBC_PTR_TO_UINT(p) static_cast<SQLUINTEGER>(reinterpret_cast<uintptr_t>(p))
#define ODBC_PTR_TO_ULEN(p) static_cast<SQLULEN>(reinterpret_cast<uintptr_t>(p))

namespace scratchbird {
namespace odbc {

namespace {
std::atomic<uint64_t> kConnectionIdCounter{0};

constexpr const char* kMetaBrowseDsnKey = "DSN";
constexpr const char* kMetaBrowseCatalogKey = "CATALOG";
constexpr const char* kMetaBrowseSchemaKey = "SCHEMA";
constexpr const char* kMetaBrowseTableKey = "TABLE";
constexpr const char* kMetaBrowseColumnKey = "COLUMN";
constexpr const char* kMetaBrowseDatabaseKey = "DATABASE";
constexpr SQLLEN kDataAtExecLenOffset = -100;

struct BrowseStage {
    bool has_dsn{false};
    bool has_catalog{false};
    bool has_schema{false};
    bool has_table{false};
    bool has_column{false};
    std::string dsn;
    std::string catalog;
    std::string schema;
    std::string table;
    std::string column;
};

struct IniSection {
    std::map<std::string, std::string> entries;
};

std::string toLower(const std::string& value);
std::string trimString(const std::string& value);
std::vector<std::string> splitPaths(const std::string& value);
std::vector<std::string> getOdbcIniPaths();
bool parseIniFile(const std::string& path, std::map<std::string, IniSection>& sections);
bool loadIniSection(const std::string& section_name, std::map<std::string, std::string>& entries);

std::string canonicalBrowseKey(const std::string& key) {
    if (key.empty()) {
        return key;
    }
    auto lower = toLower(key);
    if (lower == "dsn") {
        return kMetaBrowseDsnKey;
    }
    if (lower == "catalog" || lower == "database") {
        return kMetaBrowseCatalogKey;
    }
    if (lower == "schema") {
        return kMetaBrowseSchemaKey;
    }
    if (lower == "table") {
        return kMetaBrowseTableKey;
    }
    if (lower == "column") {
        return kMetaBrowseColumnKey;
    }
    return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(key[0])))) +
        key.substr(1);
}

std::string browseValue(const std::string& value) {
    if (value.empty()) {
        return value;
    }
    return value;
}

void appendBrowseField(std::string& out, const char* key, const std::string& value) {
    if (!value.empty()) {
        out += key;
        out += "=";
        out += browseValue(value);
        out += ";";
    }
}

bool getBrowseField(const std::map<std::string, std::string>& options,
                   const char* primary,
                   const char* secondary,
                   std::string& out) {
    auto primary_key = toLower(primary);
    auto secondary_key = secondary ? toLower(secondary) : std::string();

    auto it = options.find(primary_key);
    if (it != options.end() && !it->second.empty()) {
        out = it->second;
        return true;
    }
    if (!secondary) {
        return false;
    }
    it = options.find(secondary_key);
    if (it != options.end() && !it->second.empty()) {
        out = it->second;
        return true;
    }
    return false;
}

bool isDataAtExecIndicatorValue(SQLLEN indicator) {
    if (indicator == SQL_DATA_AT_EXEC) {
        return true;
    }
    return indicator <= kDataAtExecLenOffset && indicator != SQL_NULL_DATA &&
           indicator != SQL_NTS;
}

bool parseDataAtExecLength(SQLLEN indicator, SQLLEN* out_length) {
    if (indicator == SQL_DATA_AT_EXEC) {
        return false;
    }

    if (indicator <= kDataAtExecLenOffset && indicator != SQL_NTS &&
        indicator != SQL_NULL_DATA) {
        auto length = -indicator - 100;
        if (length < 0) {
            return false;
        }
        *out_length = length;
        return true;
    }

    return false;
}

std::string compactPreparedSql(const std::string& sql) {
    std::string result;
    result.reserve(sql.size());

    bool in_single_quote = false;
    bool in_double_quote = false;

    for (size_t index = 0; index < sql.size();) {
        const char ch = sql[index];
        if (ch == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            result.push_back(ch);
            ++index;
            continue;
        }
        if (ch == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            result.push_back(ch);
            ++index;
            continue;
        }

        if (ch == ',' && !in_single_quote && !in_double_quote) {
            result.push_back(ch);
            ++index;
            while (index < sql.size() && std::isspace(static_cast<unsigned char>(sql[index])) &&
                   sql[index] != '\n' && sql[index] != '\r') {
                ++index;
            }
            continue;
        }

        result.push_back(ch);
        ++index;
    }

    return result;
}

std::vector<std::string> discoverIniDsns() {
    auto parseDataSourceSection = [](const std::string& path, std::set<std::string>& names) {
        std::ifstream file(path);
        if (!file) {
            return;
        }

        bool in_data_sources = false;
        std::string line;
        while (std::getline(file, line)) {
            std::string trimmed = trimString(line);
            if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') {
                continue;
            }
            if (trimmed.front() == '[' && trimmed.back() == ']') {
                const auto section_name = toLower(trimString(trimmed.substr(1, trimmed.size() - 2)));
                in_data_sources = (section_name == toLower("odbc data sources"));
                continue;
            }
            if (!in_data_sources) {
                continue;
            }
            size_t eq = trimmed.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string key = trimString(trimmed.substr(0, eq));
            if (!key.empty()) {
                names.insert(key);
            }
        }
    };

    std::set<std::string> names;
    for (const auto& path : getOdbcIniPaths()) {
        parseDataSourceSection(path, names);
    }
    return {names.begin(), names.end()};
}

bool copyBrowseResponse(std::string response, SQLCHAR* out_conn_str, SQLSMALLINT out_buffer_len,
                        SQLSMALLINT* out_conn_str_len, bool need_data,
                        SQLRETURN& result, OdbcConnection* conn) {
    if (out_conn_str_len) {
        *out_conn_str_len = static_cast<SQLSMALLINT>(response.size());
    }
    if (out_buffer_len <= 0) {
        if (out_conn_str && !response.empty()) {
            out_conn_str[0] = '\0';
        }
        if (need_data) {
            conn->setError("01004", 0, "String data, right truncated");
            result = SQL_SUCCESS_WITH_INFO;
        }
        return need_data;
    }
    if (!out_conn_str) {
        if (need_data) {
            result = SQL_SUCCESS_WITH_INFO;
        }
        return need_data;
    }
    size_t copy_len = 0;
    if (response.empty()) {
        out_conn_str[0] = '\0';
        result = need_data ? SQL_NEED_DATA : SQL_SUCCESS;
        return need_data;
    }
    if (out_buffer_len > 0) {
        if (out_buffer_len > 1) {
            copy_len = std::min(static_cast<size_t>(out_buffer_len - 1), response.size());
            std::memcpy(out_conn_str, response.c_str(), copy_len);
        }
        out_conn_str[copy_len] = '\0';
    }
    if (response.size() >= static_cast<size_t>(out_buffer_len)) {
        conn->setError("01004", 0, "String data, right truncated");
        result = SQL_SUCCESS_WITH_INFO;
    } else if (need_data) {
        result = SQL_NEED_DATA;
    } else {
        result = SQL_SUCCESS;
    }
    return true;
}
const char* mapStatusToSqlState(core::Status status) {
    switch (status) {
        case core::Status::OK:
            return "00000";
        case core::Status::FILE_NOT_FOUND:
            return "HY000";
        case core::Status::FILE_EXISTS:
            return "HY000";
        case core::Status::IO_ERROR:
            return "58030";
        case core::Status::INVALID_PATH:
            return "HY000";
        case core::Status::PERMISSION_DENIED:
            return "42501";
        case core::Status::PAGE_CORRUPT:
        case core::Status::CHECKSUM_MISMATCH:
        case core::Status::INDEX_CORRUPTED:
        case core::Status::DATA_CORRUPTED:
            return "XX001";
        case core::Status::CONNECTION_FAILURE:
            return "08001";
        case core::Status::CONNECTION_DOES_NOT_EXIST:
            return "08003";
        case core::Status::PROTOCOL_VIOLATION:
            return "08S01";
        case core::Status::TOO_MANY_CONNECTIONS:
            return "08004";
        case core::Status::INVALID_PASSWORD:
        case core::Status::INVALID_AUTHORIZATION:
            return "28000";
        case core::Status::INVALID_TRANSACTION_STATE:
        case core::Status::NO_ACTIVE_TRANSACTION:
        case core::Status::TRANSACTION_ABORTED:
            return "25000";
        case core::Status::READ_ONLY_TRANSACTION:
            return "25006";
        case core::Status::DEADLOCK:
        case core::Status::LOCK_CONFLICT:
        case core::Status::SERIALIZATION_FAILURE:
            return "40001";
        case core::Status::LOCK_TIMEOUT:
            return "HYT00";
        case core::Status::CANCELLED:
        case core::Status::QUERY_CANCELED:
            return "HY008";
        case core::Status::OOM:
            return "HY001";
        case core::Status::PAGE_FULL:
        case core::Status::DISK_FULL:
            return "53100";
        case core::Status::NOT_FOUND:
        case core::Status::NO_DATA_FOUND:
            return "02000";
        case core::Status::NOT_IMPLEMENTED:
        case core::Status::NOT_SUPPORTED:
            return "HYC00";
        case core::Status::TYPE_MISMATCH:
        case core::Status::DATATYPE_MISMATCH:
            return "22005";
        case core::Status::SYNTAX_ERROR:
            return "42000";
        case core::Status::UNDEFINED_TABLE:
            return "42S02";
        case core::Status::UNDEFINED_COLUMN:
            return "42S22";
        case core::Status::UNDEFINED_FUNCTION:
            return "42883";
        case core::Status::UNDEFINED_OBJECT:
        case core::Status::INDEX_NOT_FOUND:
            return "42704";
        case core::Status::DUPLICATE_TABLE:
            return "42S01";
        case core::Status::DUPLICATE_COLUMN:
            return "42S21";
        case core::Status::DUPLICATE_OBJECT:
            return "42710";
        case core::Status::AMBIGUOUS_COLUMN:
        case core::Status::AMBIGUOUS_FUNCTION:
            return "42702";
        case core::Status::WRONG_OBJECT_TYPE:
            return "42809";
        case core::Status::INSUFFICIENT_PRIVILEGE:
            return "42501";
        case core::Status::INVALID_CURSOR_STATE:
        case core::Status::CURSOR_ALREADY_OPEN:
        case core::Status::CURSOR_NOT_OPEN:
            return "24000";
        case core::Status::INVALID_CURSOR_NAME:
        case core::Status::CURSOR_NOT_FOUND:
            return "34000";
        case core::Status::TOO_MANY_ROWS:
            return "21000";
        case core::Status::RAISE_EXCEPTION:
        case core::Status::ASSERT_FAILURE:
            return "P0001";
        case core::Status::CONSTRAINT_VIOLATION:
            return "23000";
        case core::Status::NOT_NULL_VIOLATION:
            return "23502";
        case core::Status::FOREIGN_KEY_VIOLATION:
            return "23503";
        case core::Status::UNIQUE_VIOLATION:
            return "23505";
        case core::Status::CHECK_VIOLATION:
            return "23514";
        case core::Status::EXCLUSION_VIOLATION:
            return "23P01";
        case core::Status::DIVISION_BY_ZERO:
            return "22012";
        case core::Status::NUMERIC_VALUE_OUT_OF_RANGE:
        case core::Status::OUT_OF_RANGE:
            return "22003";
        case core::Status::STRING_DATA_RIGHT_TRUNCATION:
            return "22001";
        case core::Status::DATETIME_FIELD_OVERFLOW:
            return "22008";
        case core::Status::INVALID_DATETIME_FORMAT:
            return "22007";
        case core::Status::INVALID_TEXT_REPRESENTATION:
            return "22018";
        case core::Status::NULL_VALUE_NOT_ALLOWED:
            return "22004";
        case core::Status::CONFIGURATION_LIMIT_EXCEEDED:
        case core::Status::STATEMENT_TOO_COMPLEX:
        case core::Status::TOO_MANY_COLUMNS:
            return "54000";
        case core::Status::COMPRESSION_ERROR:
        case core::Status::INTERNAL_ERROR:
            return "HY000";
        case core::Status::ADMIN_SHUTDOWN:
        case core::Status::CRASH_SHUTDOWN:
        case core::Status::DATABASE_DROPPED:
            return "08006";
        case core::Status::OBJECT_IN_USE:
            return "55006";
        case core::Status::LOCK_NOT_AVAILABLE:
            return "55P03";
        case core::Status::INVALID_ARGUMENT:
            return "HY009";
        default:
            return "HY000";
    }
}

std::string trimString(const std::string& value) {
    size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::string quoteSqlLiteral(const std::string& value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('\'');
    for (char ch : value) {
        if (ch == '\'') {
            quoted.push_back('\'');
        }
        quoted.push_back(ch);
    }
    quoted.push_back('\'');
    return quoted;
}

std::vector<std::string> splitSqlStatements(const std::string& sql) {
    std::vector<std::string> statements;
    std::string current;
    current.reserve(sql.size());

    bool in_single = false;
    bool in_double = false;

    for (size_t i = 0; i < sql.size(); ++i) {
        char ch = sql[i];

        if (in_single) {
            current.push_back(ch);
            if (ch == '\'') {
                if (i + 1 < sql.size() && sql[i + 1] == '\'') {
                    current.push_back(sql[++i]);
                } else {
                    in_single = false;
                }
            }
            continue;
        }

        if (in_double) {
            current.push_back(ch);
            if (ch == '"') {
                if (i + 1 < sql.size() && sql[i + 1] == '"') {
                    current.push_back(sql[++i]);
                } else {
                    in_double = false;
                }
            }
            continue;
        }

        if (ch == '\'') {
            in_single = true;
            current.push_back(ch);
            continue;
        }

        if (ch == '"') {
            in_double = true;
            current.push_back(ch);
            continue;
        }

        if (ch == ';') {
            std::string trimmed = trimString(current);
            if (!trimmed.empty()) {
                statements.push_back(std::move(trimmed));
            }
            current.clear();
            continue;
        }

        current.push_back(ch);
    }

    std::string trimmed = trimString(current);
    if (!trimmed.empty()) {
        statements.push_back(std::move(trimmed));
    }

    return statements;
}

std::string toUpper(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string toLower(const std::string& value) {
    auto output = value;
    for (char& ch : output) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return output;
}

bool normalizeNativeProtocol(const std::string& input, std::string& out) {
    std::string lower = toLower(trimString(input));
    if (lower.empty() || lower == "native" || lower == "scratchbird" ||
        lower == "scratchbird-native" || lower == "scratchbird_native") {
        out = "native";
        return true;
    }
    return false;
}

std::string formatDateStruct(const SQL_DATE_STRUCT& date) {
    std::ostringstream oss;
    oss << std::setw(4) << std::setfill('0') << static_cast<int>(date.year)
        << "-" << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(date.month)
        << "-" << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(date.day);
    return oss.str();
}

std::string formatTimeStruct(const SQL_TIME_STRUCT& time) {
    std::ostringstream oss;
    oss << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(time.hour)
        << ":" << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(time.minute)
        << ":" << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(time.second);
    return oss.str();
}

std::string formatTimestampStruct(const SQL_TIMESTAMP_STRUCT& ts) {
    unsigned int micros = static_cast<unsigned int>(ts.fraction / 1000);
    std::ostringstream oss;
    oss << std::setw(4) << std::setfill('0') << static_cast<int>(ts.year)
        << "-" << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(ts.month)
        << "-" << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(ts.day)
        << " " << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(ts.hour)
        << ":" << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(ts.minute)
        << ":" << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(ts.second);
    if (micros > 0) {
        oss << "." << std::setw(6) << std::setfill('0') << micros;
    }
    return oss.str();
}

std::string formatGuidStruct(const SQLGUID& guid) {
    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  guid.Data1,
                  guid.Data2,
                  guid.Data3,
                  guid.Data4[0], guid.Data4[1],
                  guid.Data4[2], guid.Data4[3], guid.Data4[4],
                  guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return std::string(buf);
}

std::vector<std::string> splitBrowsePath(const std::string& path) {
    std::vector<std::string> parts;
    if (path.empty()) {
        return parts;
    }

    // Prefer slash-delimited browse paths so dotted schema names remain intact.
    const bool use_dot_delimiter = path.find('/') == std::string::npos;
    const char delimiter = use_dot_delimiter ? '.' : '/';

    std::string part;
    for (char ch : path) {
        if (ch == delimiter) {
            part = trimString(part);
            if (!part.empty()) {
                parts.push_back(part);
            }
            part.clear();
            continue;
        }
        part.push_back(ch);
    }

    part = trimString(part);
    if (!part.empty()) {
        parts.push_back(part);
    }

    return parts;
}

void parseBrowseInput(const std::string& in_conn_str, SQLSMALLINT in_conn_str_len,
                     std::map<std::string, std::string>& out) {
    out.clear();
    if (in_conn_str.empty()) {
        return;
    }
    std::string conn_string = in_conn_str;
    if (in_conn_str_len > 0 && in_conn_str_len != SQL_NTS) {
        conn_string = std::string(in_conn_str.data(), static_cast<size_t>(in_conn_str_len));
    }

    if (conn_string.empty()) {
        return;
    }

    scratchbird::client::parseKeyValueConnectionString(conn_string, out, nullptr);

    if (out.empty()) {
        const size_t end = conn_string.find(';');
        const auto fallback_path = trimString(
            (end == std::string::npos) ? conn_string : conn_string.substr(0, end));
        if (!fallback_path.empty()) {
            out["path"] = fallback_path;
        }
    }
}

void deriveBrowseStageFromPath(const std::string& path, BrowseStage& stage) {
    const auto parts = splitBrowsePath(path);
    if (parts.empty()) {
        return;
    }
    if (!stage.has_dsn && parts.size() >= 1) {
        stage.dsn = parts[0];
        stage.has_dsn = true;
    }
    if (!stage.has_catalog && parts.size() >= 2) {
        stage.catalog = parts[1];
        stage.has_catalog = true;
    }
    if (!stage.has_schema && parts.size() >= 3) {
        stage.schema = parts[2];
        stage.has_schema = true;
    }
    if (!stage.has_table && parts.size() >= 4) {
        stage.table = parts[3];
        stage.has_table = true;
    }
    if (!stage.has_column && parts.size() >= 5) {
        stage.column = parts[4];
        stage.has_column = true;
    }
}

BrowseStage parseBrowseStage(const std::map<std::string, std::string>& options) {
    BrowseStage stage;
    std::string catalog;
    std::string path;
    if (getBrowseField(options, "dsn", nullptr, stage.dsn)) {
        stage.has_dsn = !stage.dsn.empty();
    }
    if (getBrowseField(options, kMetaBrowseCatalogKey, kMetaBrowseDatabaseKey, stage.catalog)) {
        stage.has_catalog = !stage.catalog.empty();
    }
    if (getBrowseField(options, kMetaBrowseSchemaKey, "currentschema", stage.schema)) {
        stage.has_schema = !stage.schema.empty();
    }
    if (getBrowseField(options, kMetaBrowseTableKey, "table_name", stage.table)) {
        stage.has_table = !stage.table.empty();
    }
    if (getBrowseField(options, kMetaBrowseColumnKey, "column_name", stage.column)) {
        stage.has_column = !stage.column.empty();
    }
    if (getBrowseField(options, "path", nullptr, path)) {
        deriveBrowseStageFromPath(path, stage);
    }

    if (!stage.has_dsn && !path.empty()) {
        deriveBrowseStageFromPath(path, stage);
    }

    return stage;
}

BrowseStage parseBrowseStage(const SQLCHAR* in_conn_str, SQLSMALLINT in_conn_str_len) {
    std::string input;
    if (in_conn_str) {
        input = (in_conn_str_len == SQL_NTS) ?
            std::string(reinterpret_cast<const char*>(in_conn_str)) :
            std::string(reinterpret_cast<const char*>(in_conn_str), in_conn_str_len);
    }
    std::map<std::string, std::string> options;
    parseBrowseInput(input, in_conn_str_len, options);
    return parseBrowseStage(options);
}

void appendPersistentConnectionAttributes(const std::map<std::string, std::string>& options,
                                        std::string& response) {
    for (const auto& entry : options) {
        auto key = toLower(entry.first);
        if (key == toLower(kMetaBrowseDsnKey) ||
            key == toLower(kMetaBrowseCatalogKey) ||
            key == toLower(kMetaBrowseDatabaseKey) ||
            key == toLower(kMetaBrowseSchemaKey) ||
            key == toLower(kMetaBrowseTableKey) ||
            key == toLower(kMetaBrowseColumnKey) ||
            key == "path" ||
            key == "table_name" ||
            key == "column_name" ||
            key == "currentschema") {
            continue;
        }

        appendBrowseField(response, canonicalBrowseKey(entry.first).c_str(), entry.second);
    }
}

std::vector<std::string> splitPaths(const std::string& value) {
    std::vector<std::string> parts;
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    size_t start = 0;
    while (start <= value.size()) {
        size_t pos = value.find(separator, start);
        if (pos == std::string::npos) {
            pos = value.size();
        }
        std::string part = trimString(value.substr(start, pos - start));
        if (!part.empty()) {
            parts.push_back(part);
        }
        if (pos == value.size()) {
            break;
        }
        start = pos + 1;
    }
    return parts;
}

void addIniPath(std::vector<std::string>& paths, const std::string& path) {
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        paths.push_back(path);
    }
}

std::vector<std::string> getOdbcIniPaths() {
    std::vector<std::string> paths;
    const char* odbcini_env = std::getenv("ODBCINI");
    if (odbcini_env && *odbcini_env) {
        for (const auto& path : splitPaths(odbcini_env)) {
            addIniPath(paths, path);
        }
        return paths;
    }

#ifdef _WIN32
    return paths;
#else
    const char* odbc_sys = std::getenv("ODBCSYSINI");
    if (odbc_sys && *odbc_sys) {
        addIniPath(paths, std::string(odbc_sys) + "/odbc.ini");
    }
    addIniPath(paths, "/etc/odbc.ini");

    const char* home = std::getenv("HOME");
    if (home && *home) {
        addIniPath(paths, std::string(home) + "/.odbc.ini");
        addIniPath(paths, std::string(home) + "/Library/ODBC/odbc.ini");
    }
#endif

    return paths;
}

bool parseIniFile(const std::string& path, std::map<std::string, IniSection>& sections) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }

    std::string current_section;
    std::string line;
    while (std::getline(file, line)) {
        std::string trimmed = trimString(line);
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') {
            continue;
        }

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            current_section = toLower(trimString(trimmed.substr(1, trimmed.size() - 2)));
            continue;
        }

        size_t eq = trimmed.find('=');
        if (eq == std::string::npos || current_section.empty()) {
            continue;
        }

        std::string key = toLower(trimString(trimmed.substr(0, eq)));
        std::string value = trimString(trimmed.substr(eq + 1));
        if (!key.empty()) {
            sections[current_section].entries[key] = value;
        }
    }

    return true;
}

bool loadIniSection(const std::string& section_name, std::map<std::string, std::string>& entries) {
    if (section_name.empty()) {
        return false;
    }
    std::string section_key = toLower(section_name);
    for (const auto& path : getOdbcIniPaths()) {
        std::map<std::string, IniSection> sections;
        if (!parseIniFile(path, sections)) {
            continue;
        }
        auto it = sections.find(section_key);
        if (it != sections.end()) {
            entries = it->second.entries;
            return true;
        }
    }
    return false;
}

constexpr SQLUINTEGER kSqlConformanceEntry =
#ifdef SQL_SC_SQL92_ENTRY
    SQL_SC_SQL92_ENTRY;
#else
    1;
#endif

constexpr SQLUSMALLINT kOdbcApiLevel1 =
#ifdef SQL_OAC_LEVEL1
    SQL_OAC_LEVEL1;
#else
    1;
#endif

constexpr SQLUSMALLINT kOdbcSqlCore =
#ifdef SQL_OSC_CORE
    SQL_OSC_CORE;
#else
    1;
#endif

std::string buildAutocommitSql(SQLUINTEGER mode) {
    if (mode == SQL_AUTOCOMMIT_ON) {
        return "SET AUTOCOMMIT ON ON CONFLICT COMMIT";
    }
    return "SET AUTOCOMMIT OFF ON CONFLICT KEEP";
}

bool buildIsolationSql(SQLUINTEGER isolation, std::string& out_sql) {
    if (isolation == 0 || (isolation & (isolation - 1)) != 0) {
        return false;
    }

    // ODBC exposes SQL-standard isolation aliases only.
    // READ_UNCOMMITTED remains a legacy compatibility alias here, not a
    // distinct canonical MGA mode. READ_COMMITTED maps to canonical
    // READ COMMITTED, REPEATABLE_READ / VERSIONING map to canonical SNAPSHOT,
    // and SERIALIZABLE maps to canonical SNAPSHOT TABLE STABILITY. A distinct
    // READ COMMITTED READ CONSISTENCY selector is not exposed via ODBC's
    // standard SQL_ATTR_TXN_ISOLATION surface.
    switch (isolation) {
        case SQL_TXN_READ_UNCOMMITTED:
            out_sql = "SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED ON CONFLICT COMMIT";
            return true;
        case SQL_TXN_READ_COMMITTED:
            out_sql = "SET TRANSACTION ISOLATION LEVEL READ COMMITTED ON CONFLICT COMMIT";
            return true;
        case SQL_TXN_REPEATABLE_READ:
            out_sql = "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ ON CONFLICT COMMIT";
            return true;
        case SQL_TXN_SERIALIZABLE:
            out_sql = "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE ON CONFLICT COMMIT";
            return true;
#ifdef SQL_TXN_VERSIONING
        case SQL_TXN_VERSIONING:
            out_sql = "SET TRANSACTION ISOLATION LEVEL SNAPSHOT ON CONFLICT COMMIT";
            return true;
#endif
        default:
            return false;
    }
}

std::string sqlCharToString(const SQLCHAR* value, SQLSMALLINT length) {
    if (!value) {
        return "";
    }
    if (length == SQL_NTS) {
        return std::string(reinterpret_cast<const char*>(value));
    }
    if (length <= 0) {
        return "";
    }
    return std::string(reinterpret_cast<const char*>(value), length);
}

std::string escapeRegexChar(char ch) {
    switch (ch) {
        case '.': case '^': case '$': case '|': case '(':
        case ')': case '[': case ']': case '{': case '}':
        case '*': case '+': case '?': case '\\':
            return std::string("\\") + ch;
        default:
            return std::string(1, ch);
    }
}

bool matchPattern(const std::string& value, const std::string& pattern, bool metadata_id) {
    if (pattern.empty()) {
        return true;
    }
    if (metadata_id) {
        return value == pattern;
    }

    std::string regex = "^";
    bool escape = false;
    for (char ch : pattern) {
        if (escape) {
            regex += escapeRegexChar(ch);
            escape = false;
            continue;
        }
        if (ch == '\\') {
            escape = true;
            continue;
        }
        if (ch == '%') {
            regex += ".*";
        } else if (ch == '_') {
            regex += '.';
        } else {
            regex += escapeRegexChar(ch);
        }
    }
    regex += "$";

    try {
        return std::regex_match(value, std::regex(regex));
    } catch (const std::regex_error&) {
        return value == pattern;
    }
}

bool parseInt64(const std::string& value, int64_t& out);

bool parseBoolValue(const std::string& value, bool default_value = false) {
    if (value.empty()) {
        return default_value;
    }
    int64_t numeric = 0;
    if (parseInt64(value, numeric)) {
        return numeric != 0;
    }
    std::string upper = toUpper(trimString(value));
    return upper == "YES" || upper == "Y" || upper == "TRUE" || upper == "T";
}

bool isBinarySqlType(SQLSMALLINT type) {
    switch (type) {
        case SQL_BINARY:
        case SQL_VARBINARY:
        case SQL_LONGVARBINARY:
            return true;
        default:
            return false;
    }
}

bool isCharacterSqlType(SQLSMALLINT type) {
    switch (type) {
        case SQL_CHAR:
        case SQL_VARCHAR:
        case SQL_LONGVARCHAR:
            return true;
        default:
            return false;
    }
}

std::string bytesToHexString(const std::string& data) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(data.size() * 2);
    for (unsigned char ch : data) {
        out.push_back(kHex[(ch >> 4) & 0xF]);
        out.push_back(kHex[ch & 0xF]);
    }
    return out;
}

constexpr SQLSMALLINT kOdbcFkCascade = 0;
constexpr SQLSMALLINT kOdbcFkRestrict = 1;
constexpr SQLSMALLINT kOdbcFkSetNull = 2;
constexpr SQLSMALLINT kOdbcFkNoAction = 3;
constexpr SQLSMALLINT kOdbcFkSetDefault = 4;
constexpr SQLSMALLINT kOdbcNotDeferrable = 7;
constexpr SQLSMALLINT kOdbcInitiallyDeferred = 5;
constexpr SQLSMALLINT kOdbcInitiallyImmediate = 6;

bool parseInt64(const std::string& value, int64_t& out) {
    std::string trimmed = trimString(value);
    if (trimmed.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    long long parsed = std::strtoll(trimmed.c_str(), &end, 10);
    if (errno != 0 || end == trimmed.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<int64_t>(parsed);
    return true;
}

bool parseUInt32(const std::string& value, uint32_t& out) {
    std::string trimmed = trimString(value);
    if (trimmed.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    unsigned long parsed = std::strtoul(trimmed.c_str(), &end, 10);
    if (errno != 0 || end == trimmed.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<uint32_t>(parsed);
    return true;
}

std::string stripIdentifierQuotes(const std::string& value) {
    if (value.size() < 2) {
        return value;
    }
    char first = value.front();
    char last = value.back();
    if ((first == '`' && last == '`') || (first == '"' && last == '"')) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

std::vector<std::string> splitCsvColumns(const std::string& value) {
    std::vector<std::string> columns;
    std::stringstream ss(value);
    std::string token;
    while (std::getline(ss, token, ',')) {
        std::string trimmed = stripIdentifierQuotes(trimString(token));
        if (!trimmed.empty()) {
            columns.push_back(trimmed);
        }
    }
    return columns;
}

struct PrivilegeObjectPath {
    std::string full;
    std::string schema;
    std::string table;
    std::string column;
    bool has_column{false};
};

std::vector<std::string> splitQualifiedIdentifierPath(const std::string& value) {
    std::vector<std::string> parts;
    std::string current;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool in_backtick_quote = false;

    for (size_t index = 0; index < value.size(); ++index) {
        char ch = value[index];

        if (ch == '\'' && !in_double_quote && !in_backtick_quote) {
            in_single_quote = !in_single_quote;
            continue;
        }
        if (ch == '"' && !in_single_quote && !in_backtick_quote) {
            in_double_quote = !in_double_quote;
            continue;
        }
        if (ch == '`' && !in_single_quote && !in_double_quote) {
            in_backtick_quote = !in_backtick_quote;
            continue;
        }

        if (!in_single_quote && !in_double_quote && !in_backtick_quote && ch == '.') {
            std::string part = trimString(current);
            if (!part.empty()) {
                parts.push_back(part);
            }
            current.clear();
            continue;
        }

        current.push_back(ch);
    }

    std::string part = trimString(current);
    if (!part.empty()) {
        parts.push_back(part);
    }

    return parts;
}

PrivilegeObjectPath parsePrivilegeObjectPath(const std::string& object_name) {
    PrivilegeObjectPath parsed;
    parsed.full = stripIdentifierQuotes(trimString(object_name));
    if (parsed.full.empty()) {
        return parsed;
    }

    const auto path_parts = splitQualifiedIdentifierPath(parsed.full);
    if (path_parts.empty()) {
        return parsed;
    }

    if (path_parts.size() == 1) {
        parsed.table = stripIdentifierQuotes(path_parts[0]);
        return parsed;
    }

    if (path_parts.size() == 2) {
        parsed.schema = stripIdentifierQuotes(path_parts[0]);
        parsed.table = stripIdentifierQuotes(path_parts[1]);
        return parsed;
    }

    parsed.has_column = true;
    for (size_t index = 0; index + 2 < path_parts.size(); ++index) {
        if (!parsed.schema.empty()) {
            parsed.schema.push_back('.');
        }
        parsed.schema += stripIdentifierQuotes(path_parts[index]);
    }
    parsed.table = stripIdentifierQuotes(path_parts[path_parts.size() - 2]);
    parsed.column = stripIdentifierQuotes(path_parts.back());
    return parsed;
}

bool isRoleGrantObject(const std::string& object_name, const std::string& privilege) {
    if (toUpper(trimString(privilege)) == "ROLE") {
        return true;
    }
    return toUpper(trimString(object_name)).rfind("ROLE ", 0) == 0;
}

std::string normalizeGrantOption(const std::string& value) {
    if (parseBoolValue(value, false)) {
        return "YES";
    }
    return "NO";
}

SQLRETURN queryShowGrants(OdbcConnection* conn, std::vector<std::vector<std::string>>& rows) {
    std::vector<ColumnMetadata> columns;
    SQLLEN rows_affected = 0;
    return conn->executeSQL("SHOW GRANTS", rows, columns, rows_affected);
}

bool matchTablePattern(const PrivilegeObjectPath& path,
                      const std::string& table_pattern,
                      bool metadata_id) {
    if (table_pattern.empty()) {
        return true;
    }
    return matchPattern(path.table, table_pattern, metadata_id);
}

bool matchSchemaPattern(const PrivilegeObjectPath& path,
                       const std::string& schema_pattern,
                       bool metadata_id) {
    if (schema_pattern.empty()) {
        return true;
    }
    return matchPattern(path.schema, schema_pattern, metadata_id);
}

SQLRETURN executeCatalogQuery(OdbcConnection* conn,
                              const std::vector<std::string>& queries,
                              std::vector<std::vector<std::string>>& rows,
                              std::vector<ColumnMetadata>& columns,
                              SQLLEN& rows_affected) {
    SQLRETURN status = SQL_ERROR;
    for (const auto& query : queries) {
        status = conn->executeSQL(query, rows, columns, rows_affected);
        if (status == SQL_SUCCESS) {
            return status;
        }
    }
    return status;
}

SQLSMALLINT mapFkRuleToOdbc(const std::string& value) {
    int64_t numeric = 0;
    if (parseInt64(value, numeric)) {
        switch (numeric) {
            case 0: return kOdbcFkNoAction;
            case 1: return kOdbcFkRestrict;
            case 2: return kOdbcFkCascade;
            case 3: return kOdbcFkSetNull;
            case 4: return kOdbcFkSetDefault;
            default: return kOdbcFkNoAction;
        }
    }

    std::string upper = toUpper(trimString(value));
    if (upper == "CASCADE") return kOdbcFkCascade;
    if (upper == "RESTRICT") return kOdbcFkRestrict;
    if (upper == "SET NULL" || upper == "SET_NULL") return kOdbcFkSetNull;
    if (upper == "SET DEFAULT" || upper == "SET_DEFAULT") return kOdbcFkSetDefault;
    if (upper == "NO ACTION" || upper == "NO_ACTION") return kOdbcFkNoAction;
    return kOdbcFkNoAction;
}

SQLSMALLINT mapDeferrabilityToOdbc(const std::string& value) {
    std::string upper = toUpper(trimString(value));
    if (upper == "INITIALLY DEFERRED" || upper == "DEFERRED") {
        return kOdbcInitiallyDeferred;
    }
    if (upper == "INITIALLY IMMEDIATE" || upper == "IMMEDIATE") {
        return kOdbcInitiallyImmediate;
    }
    if (upper == "NOT DEFERRABLE" || upper == "NOT_DEFERRABLE") {
        return kOdbcNotDeferrable;
    }
    return kOdbcNotDeferrable;
}

bool parseDateYMD(const std::string& value, SQL_DATE_STRUCT& out) {
    if (value.size() < 10) {
        return false;
    }
    auto is_digit = [](char ch) { return ch >= '0' && ch <= '9'; };
    if (!is_digit(value[0]) || !is_digit(value[1]) || !is_digit(value[2]) || !is_digit(value[3]) ||
        value[4] != '-' ||
        !is_digit(value[5]) || !is_digit(value[6]) ||
        value[7] != '-' ||
        !is_digit(value[8]) || !is_digit(value[9])) {
        return false;
    }

    out.year = static_cast<SQLSMALLINT>(std::stoi(value.substr(0, 4)));
    out.month = static_cast<SQLUSMALLINT>(std::stoi(value.substr(5, 2)));
    out.day = static_cast<SQLUSMALLINT>(std::stoi(value.substr(8, 2)));
    return true;
}

bool parseTimeHMS(const std::string& value, SQL_TIME_STRUCT& out, SQLUINTEGER* fraction_ns) {
    if (value.size() < 8) {
        return false;
    }
    auto is_digit = [](char ch) { return ch >= '0' && ch <= '9'; };
    if (!is_digit(value[0]) || !is_digit(value[1]) || value[2] != ':' ||
        !is_digit(value[3]) || !is_digit(value[4]) || value[5] != ':' ||
        !is_digit(value[6]) || !is_digit(value[7])) {
        return false;
    }

    out.hour = static_cast<SQLUSMALLINT>(std::stoi(value.substr(0, 2)));
    out.minute = static_cast<SQLUSMALLINT>(std::stoi(value.substr(3, 2)));
    out.second = static_cast<SQLUSMALLINT>(std::stoi(value.substr(6, 2)));

    if (fraction_ns) {
        *fraction_ns = 0;
        if (value.size() > 8 && value[8] == '.') {
            std::string frac = value.substr(9);
            if (frac.size() > 9) {
                frac.resize(9);
            }
            SQLUINTEGER nanos = 0;
            for (char ch : frac) {
                if (!is_digit(ch)) {
                    break;
                }
                nanos = nanos * 10 + static_cast<SQLUINTEGER>(ch - '0');
            }
            for (size_t i = frac.size(); i < 9; ++i) {
                nanos *= 10;
            }
            *fraction_ns = nanos;
        }
    }
    return true;
}

bool parseDateLiteral(const std::string& value, SQL_DATE_STRUCT& out) {
    std::string trimmed = trimString(value);
    if (trimmed.rfind("DATE(", 0) == 0 && trimmed.back() == ')') {
        int64_t days = 0;
        std::string inner = trimString(trimmed.substr(5, trimmed.size() - 6));
        if (!parseInt64(inner, days)) {
            return false;
        }
        out.year = static_cast<SQLSMALLINT>(core::TypeExtractor::extractYear(days));
        out.month = static_cast<SQLUSMALLINT>(core::TypeExtractor::extractMonth(days));
        out.day = static_cast<SQLUSMALLINT>(core::TypeExtractor::extractDay(days));
        return true;
    }
    if (parseDateYMD(trimmed, out)) {
        return true;
    }
    int64_t days = 0;
    if (parseInt64(trimmed, days)) {
        out.year = static_cast<SQLSMALLINT>(core::TypeExtractor::extractYear(days));
        out.month = static_cast<SQLUSMALLINT>(core::TypeExtractor::extractMonth(days));
        out.day = static_cast<SQLUSMALLINT>(core::TypeExtractor::extractDay(days));
        return true;
    }
    return false;
}

bool parseTimeLiteral(const std::string& value, SQL_TIME_STRUCT& out) {
    std::string trimmed = trimString(value);
    if (trimmed.rfind("TIME(", 0) == 0 && trimmed.back() == ')') {
        int64_t micros = 0;
        std::string inner = trimString(trimmed.substr(5, trimmed.size() - 6));
        if (!parseInt64(inner, micros)) {
            return false;
        }
        out.hour = static_cast<SQLUSMALLINT>(core::TypeExtractor::extractHour(micros));
        out.minute = static_cast<SQLUSMALLINT>(core::TypeExtractor::extractMinute(micros));
        out.second = static_cast<SQLUSMALLINT>(core::TypeExtractor::extractSecond(micros));
        return true;
    }
    return parseTimeHMS(trimmed, out, nullptr);
}

bool parseTimestampLiteral(const std::string& value, SQL_TIMESTAMP_STRUCT& out) {
    std::string trimmed = trimString(value);
    if (trimmed.rfind("TIMESTAMP(", 0) == 0 && trimmed.back() == ')') {
        int64_t micros = 0;
        std::string inner = trimString(trimmed.substr(10, trimmed.size() - 11));
        if (!parseInt64(inner, micros)) {
            return false;
        }
        out.year = static_cast<SQLSMALLINT>(core::TypeExtractor::extractTimestampYear(micros));
        out.month = static_cast<SQLUSMALLINT>(core::TypeExtractor::extractTimestampMonth(micros));
        out.day = static_cast<SQLUSMALLINT>(core::TypeExtractor::extractTimestampDay(micros));
        out.hour = static_cast<SQLUSMALLINT>(core::TypeExtractor::extractTimestampHour(micros));
        out.minute = static_cast<SQLUSMALLINT>(core::TypeExtractor::extractTimestampMinute(micros));
        out.second = static_cast<SQLUSMALLINT>(core::TypeExtractor::extractTimestampSecond(micros));
        out.fraction = static_cast<SQLUINTEGER>(core::TypeExtractor::extractTimestampMicrosecond(micros)) * 1000;
        return true;
    }

    size_t split = trimmed.find(' ');
    if (split == std::string::npos) {
        split = trimmed.find('T');
    }
    if (split == std::string::npos) {
        return false;
    }

    SQL_DATE_STRUCT date{};
    SQL_TIME_STRUCT time{};
    SQLUINTEGER fraction = 0;
    if (!parseDateYMD(trimmed.substr(0, split), date)) {
        return false;
    }
    if (!parseTimeHMS(trimmed.substr(split + 1), time, &fraction)) {
        return false;
    }

    out.year = date.year;
    out.month = date.month;
    out.day = date.day;
    out.hour = time.hour;
    out.minute = time.minute;
    out.second = time.second;
    out.fraction = fraction;
    return true;
}

bool parseGuidString(const std::string& value, SQLGUID& out) {
    std::string hex;
    hex.reserve(32);
    for (char ch : value) {
        if (ch == '-' || ch == '{' || ch == '}') {
            continue;
        }
        if (std::isxdigit(static_cast<unsigned char>(ch))) {
            hex.push_back(ch);
        }
    }
    if (hex.size() != 32) {
        return false;
    }

    auto hexToNibble = [](char ch) -> uint8_t {
        if (ch >= '0' && ch <= '9') return static_cast<uint8_t>(ch - '0');
        if (ch >= 'a' && ch <= 'f') return static_cast<uint8_t>(10 + ch - 'a');
        if (ch >= 'A' && ch <= 'F') return static_cast<uint8_t>(10 + ch - 'A');
        return 0;
    };

    uint8_t bytes[16]{};
    for (size_t i = 0; i < 16; ++i) {
        bytes[i] = static_cast<uint8_t>((hexToNibble(hex[i * 2]) << 4) | hexToNibble(hex[i * 2 + 1]));
    }

    out.Data1 = (static_cast<uint32_t>(bytes[0]) << 24) |
                (static_cast<uint32_t>(bytes[1]) << 16) |
                (static_cast<uint32_t>(bytes[2]) << 8) |
                static_cast<uint32_t>(bytes[3]);
    out.Data2 = static_cast<uint16_t>((bytes[4] << 8) | bytes[5]);
    out.Data3 = static_cast<uint16_t>((bytes[6] << 8) | bytes[7]);
    std::memcpy(out.Data4, bytes + 8, sizeof(out.Data4));
    return true;
}

struct ParsedTypeInfo {
    SQLSMALLINT sql_type{SQL_VARCHAR};
    std::string type_name{"UNKNOWN"};
    SQLULEN column_size{0};
    SQLSMALLINT decimal_digits{0};
    SQLSMALLINT num_prec_radix{0};
};

ParsedTypeInfo parseTypeString(const std::string& type_str) {
    ParsedTypeInfo info;
    std::string trimmed = trimString(type_str);
    if (trimmed.empty()) {
        return info;
    }

    size_t paren = trimmed.find('(');
    std::string base = (paren == std::string::npos) ? trimmed : trimmed.substr(0, paren);
    base = trimString(base);
    std::string upper = toUpper(base);

    uint32_t precision = 0;
    uint32_t scale = 0;
    if (paren != std::string::npos) {
        size_t close = trimmed.find(')', paren);
        std::string args = trimmed.substr(paren + 1, close == std::string::npos ? std::string::npos : close - paren - 1);
        size_t comma = args.find(',');
        if (comma == std::string::npos) {
            parseUInt32(args, precision);
        } else {
            parseUInt32(args.substr(0, comma), precision);
            parseUInt32(args.substr(comma + 1), scale);
        }
    }

    if (upper == "TINYINT") {
        info.sql_type = SQL_TINYINT;
    } else if (upper == "SMALLINT" || upper == "INT2") {
        info.sql_type = SQL_SMALLINT;
    } else if (upper == "INT" || upper == "INTEGER" || upper == "INT4") {
        info.sql_type = SQL_INTEGER;
    } else if (upper == "BIGINT" || upper == "INT8") {
        info.sql_type = SQL_BIGINT;
    } else if (upper == "FLOAT" || upper == "REAL" || upper == "FLOAT4") {
        info.sql_type = SQL_REAL;
    } else if (upper == "DOUBLE" || upper == "DOUBLE PRECISION" || upper == "FLOAT8") {
        info.sql_type = SQL_DOUBLE;
    } else if (upper == "DECIMAL" || upper == "NUMERIC" || upper == "MONEY") {
        info.sql_type = SQL_DECIMAL;
    } else if (upper == "BOOLEAN" || upper == "BOOL") {
        info.sql_type = SQL_BIT;
    } else if (upper == "CHAR" || upper == "BPCHAR") {
        info.sql_type = SQL_CHAR;
    } else if (upper == "VARCHAR") {
        info.sql_type = SQL_VARCHAR;
    } else if (upper == "TEXT" || upper == "TSVECTOR" || upper == "TSQUERY" ||
               upper == "RECORD" || upper == "INT4RANGE" || upper == "INT8RANGE" ||
               upper == "NUMRANGE" || upper == "TSRANGE" || upper == "TSTZRANGE" ||
               upper == "DATERANGE" || upper == "ARRAY" || upper == "COMPOSITE") {
        info.sql_type = SQL_LONGVARCHAR;
    } else if (upper == "BINARY") {
        info.sql_type = SQL_BINARY;
    } else if (upper == "VARBINARY") {
        info.sql_type = SQL_VARBINARY;
    } else if (upper == "BLOB" || upper == "BYTEA" || upper == "VECTOR" ||
               upper == "POINT" || upper == "LSEG" || upper == "PATH" ||
               upper == "BOX" || upper == "POLYGON" || upper == "LINE" ||
               upper == "CIRCLE" || upper == "LINESTRING") {
        info.sql_type = SQL_LONGVARBINARY;
    } else if (upper == "DATE") {
        info.sql_type = SQL_TYPE_DATE;
    } else if (upper == "TIME") {
        info.sql_type = SQL_TYPE_TIME;
    } else if (upper == "TIMESTAMP" || upper == "TIMESTAMPTZ") {
        info.sql_type = SQL_TYPE_TIMESTAMP;
    } else if (upper == "INTERVAL" || upper == "INET" || upper == "CIDR" ||
               upper == "MACADDR" || upper == "MACADDR8") {
        info.sql_type = SQL_VARCHAR;
    } else if (upper == "UUID") {
        info.sql_type = SQL_GUID;
    } else if (upper == "JSON" || upper == "JSONB" || upper == "XML") {
        info.sql_type = SQL_LONGVARCHAR;
    } else {
        info.sql_type = SQL_VARCHAR;
    }

    info.type_name = upper;
    if (precision > 0) {
        info.column_size = precision;
    }
    if (info.sql_type == SQL_DECIMAL) {
        info.decimal_digits = static_cast<SQLSMALLINT>(scale);
        info.num_prec_radix = 10;
    } else if (info.sql_type == SQL_INTEGER || info.sql_type == SQL_SMALLINT ||
               info.sql_type == SQL_TINYINT || info.sql_type == SQL_BIGINT) {
        info.num_prec_radix = 10;
    } else if (isBinarySqlType(info.sql_type)) {
        info.num_prec_radix = 2;
    }

    return info;
}

namespace {
constexpr SQLSMALLINT kOdbcProcedureTypeUnknown = 0;
constexpr SQLSMALLINT kOdbcProcedureTypeProcedure = 1;
constexpr SQLSMALLINT kOdbcProcedureTypeFunction = 2;

constexpr SQLSMALLINT kOdbcProcedureColumnTypeUnknown = 0;
constexpr SQLSMALLINT kOdbcProcedureColumnInput = 1;
constexpr SQLSMALLINT kOdbcProcedureColumnInputOutput = 2;
constexpr SQLSMALLINT kOdbcProcedureColumnOutput = 4;
constexpr SQLSMALLINT kOdbcProcedureColumnReturn = 5;

SQLSMALLINT parseProcedureType(const std::string& routine_type) {
    auto raw = toUpper(trimString(routine_type));
    int64_t numeric_type = 0;
    if (!raw.empty() && parseInt64(raw, numeric_type)) {
        if (numeric_type == 1) {
            return kOdbcProcedureTypeFunction;
        }
        if (numeric_type == 0) {
            return kOdbcProcedureTypeProcedure;
        }
        return kOdbcProcedureTypeUnknown;
    }

    if (raw == "PROCEDURE" || raw == "PROC") {
        return kOdbcProcedureTypeProcedure;
    }
    if (raw == "FUNCTION" || raw == "FUNC") {
        return kOdbcProcedureTypeFunction;
    }
    return kOdbcProcedureTypeUnknown;
}

SQLSMALLINT parseProcedureColumnMode(const std::string& mode) {
    auto normalized = toUpper(trimString(mode));
    if (normalized.empty()) {
        return kOdbcProcedureColumnTypeUnknown;
    }

    int64_t numeric_mode = 0;
    if (parseInt64(normalized, numeric_mode)) {
        if (numeric_mode == kOdbcProcedureColumnInput) {
            return kOdbcProcedureColumnInput;
        }
        if (numeric_mode == kOdbcProcedureColumnInputOutput) {
            return kOdbcProcedureColumnInputOutput;
        }
        if (numeric_mode == kOdbcProcedureColumnOutput) {
            return kOdbcProcedureColumnOutput;
        }
        if (numeric_mode == kOdbcProcedureColumnReturn) {
            return kOdbcProcedureColumnReturn;
        }
        return kOdbcProcedureColumnTypeUnknown;
    }

    if (normalized.find("INOUT") != std::string::npos) {
        return kOdbcProcedureColumnInputOutput;
    }
    if (normalized == "IN") {
        return kOdbcProcedureColumnInput;
    }
    if (normalized == "OUT") {
        return kOdbcProcedureColumnOutput;
    }
    if (normalized == "IN OUT" || normalized == "OUTPUT") {
        return kOdbcProcedureColumnOutput;
    }
    if (normalized == "RETURN") {
        return kOdbcProcedureColumnReturn;
    }
    return kOdbcProcedureColumnTypeUnknown;
}
}

struct TypeInfoEntry {
    const char* type_name;
    SQLSMALLINT data_type;
    SQLINTEGER column_size;
    const char* literal_prefix;
    const char* literal_suffix;
    const char* create_params;
    SQLSMALLINT nullable;
    SQLSMALLINT case_sensitive;
    SQLSMALLINT searchable;
    SQLSMALLINT unsigned_attr;
    SQLSMALLINT fixed_prec_scale;
    SQLSMALLINT auto_unique;
    const char* local_type_name;
    SQLSMALLINT min_scale;
    SQLSMALLINT max_scale;
    SQLSMALLINT sql_data_type;
    SQLSMALLINT sql_datetime_sub;
    SQLSMALLINT num_prec_radix;
    SQLSMALLINT interval_precision;
};

constexpr TypeInfoEntry kTypeInfoEntries[] = {
    {"CHAR", SQL_CHAR, 255, "'", "'", "length", SQL_NULLABLE, 1, 3, 0, 0, 0, "CHAR", 0, 0, SQL_CHAR, 0, 0, 0},
    {"VARCHAR", SQL_VARCHAR, 65535, "'", "'", "length", SQL_NULLABLE, 1, 3, 0, 0, 0, "VARCHAR", 0, 0, SQL_VARCHAR, 0, 0, 0},
    {"TEXT", SQL_LONGVARCHAR, 2147483647, "'", "'", "", SQL_NULLABLE, 1, 3, 0, 0, 0, "TEXT", 0, 0, SQL_LONGVARCHAR, 0, 0, 0},
    {"JSON", SQL_LONGVARCHAR, 2147483647, "'", "'", "", SQL_NULLABLE, 1, 3, 0, 0, 0, "JSON", 0, 0, SQL_LONGVARCHAR, 0, 0, 0},
    {"JSONB", SQL_LONGVARCHAR, 2147483647, "'", "'", "", SQL_NULLABLE, 1, 3, 0, 0, 0, "JSONB", 0, 0, SQL_LONGVARCHAR, 0, 0, 0},
    {"XML", SQL_LONGVARCHAR, 2147483647, "'", "'", "", SQL_NULLABLE, 1, 3, 0, 0, 0, "XML", 0, 0, SQL_LONGVARCHAR, 0, 0, 0},
    {"TSVECTOR", SQL_LONGVARCHAR, 2147483647, "'", "'", "", SQL_NULLABLE, 1, 3, 0, 0, 0, "TSVECTOR", 0, 0, SQL_LONGVARCHAR, 0, 0, 0},
    {"TSQUERY", SQL_LONGVARCHAR, 2147483647, "'", "'", "", SQL_NULLABLE, 1, 3, 0, 0, 0, "TSQUERY", 0, 0, SQL_LONGVARCHAR, 0, 0, 0},
    {"RECORD", SQL_LONGVARCHAR, 2147483647, "'", "'", "", SQL_NULLABLE, 1, 3, 0, 0, 0, "RECORD", 0, 0, SQL_LONGVARCHAR, 0, 0, 0},
    {"INT4RANGE", SQL_LONGVARCHAR, 2147483647, "'", "'", "", SQL_NULLABLE, 1, 3, 0, 0, 0, "INT4RANGE", 0, 0, SQL_LONGVARCHAR, 0, 0, 0},
    {"INT8RANGE", SQL_LONGVARCHAR, 2147483647, "'", "'", "", SQL_NULLABLE, 1, 3, 0, 0, 0, "INT8RANGE", 0, 0, SQL_LONGVARCHAR, 0, 0, 0},
    {"NUMRANGE", SQL_LONGVARCHAR, 2147483647, "'", "'", "", SQL_NULLABLE, 1, 3, 0, 0, 0, "NUMRANGE", 0, 0, SQL_LONGVARCHAR, 0, 0, 0},
    {"TSRANGE", SQL_LONGVARCHAR, 2147483647, "'", "'", "", SQL_NULLABLE, 1, 3, 0, 0, 0, "TSRANGE", 0, 0, SQL_LONGVARCHAR, 0, 0, 0},
    {"TSTZRANGE", SQL_LONGVARCHAR, 2147483647, "'", "'", "", SQL_NULLABLE, 1, 3, 0, 0, 0, "TSTZRANGE", 0, 0, SQL_LONGVARCHAR, 0, 0, 0},
    {"DATERANGE", SQL_LONGVARCHAR, 2147483647, "'", "'", "", SQL_NULLABLE, 1, 3, 0, 0, 0, "DATERANGE", 0, 0, SQL_LONGVARCHAR, 0, 0, 0},
    {"BINARY", SQL_BINARY, 255, "0x", "", "length", SQL_NULLABLE, 0, 3, 0, 0, 0, "BINARY", 0, 0, SQL_BINARY, 0, 0, 0},
    {"VARBINARY", SQL_VARBINARY, 65535, "0x", "", "length", SQL_NULLABLE, 0, 3, 0, 0, 0, "VARBINARY", 0, 0, SQL_VARBINARY, 0, 0, 0},
    {"BLOB", SQL_LONGVARBINARY, 2147483647, "0x", "", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "BLOB", 0, 0, SQL_LONGVARBINARY, 0, 0, 0},
    {"BYTEA", SQL_LONGVARBINARY, 2147483647, "0x", "", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "BYTEA", 0, 0, SQL_LONGVARBINARY, 0, 0, 0},
    {"POINT", SQL_LONGVARBINARY, 2147483647, "0x", "", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "POINT", 0, 0, SQL_LONGVARBINARY, 0, 0, 0},
    {"LSEG", SQL_LONGVARBINARY, 2147483647, "0x", "", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "LSEG", 0, 0, SQL_LONGVARBINARY, 0, 0, 0},
    {"PATH", SQL_LONGVARBINARY, 2147483647, "0x", "", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "PATH", 0, 0, SQL_LONGVARBINARY, 0, 0, 0},
    {"BOX", SQL_LONGVARBINARY, 2147483647, "0x", "", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "BOX", 0, 0, SQL_LONGVARBINARY, 0, 0, 0},
    {"POLYGON", SQL_LONGVARBINARY, 2147483647, "0x", "", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "POLYGON", 0, 0, SQL_LONGVARBINARY, 0, 0, 0},
    {"LINE", SQL_LONGVARBINARY, 2147483647, "0x", "", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "LINE", 0, 0, SQL_LONGVARBINARY, 0, 0, 0},
    {"CIRCLE", SQL_LONGVARBINARY, 2147483647, "0x", "", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "CIRCLE", 0, 0, SQL_LONGVARBINARY, 0, 0, 0},
    {"VECTOR", SQL_LONGVARBINARY, 2147483647, "0x", "", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "VECTOR", 0, 0, SQL_LONGVARBINARY, 0, 0, 0},
    {"BOOLEAN", SQL_BIT, 1, "", "", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "BOOLEAN", 0, 0, SQL_BIT, 0, 0, 0},
    {"TINYINT", SQL_TINYINT, 3, "", "", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "TINYINT", 0, 0, SQL_TINYINT, 0, 10, 0},
    {"SMALLINT", SQL_SMALLINT, 5, "", "", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "SMALLINT", 0, 0, SQL_SMALLINT, 0, 10, 0},
    {"INTEGER", SQL_INTEGER, 10, "", "", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "INTEGER", 0, 0, SQL_INTEGER, 0, 10, 0},
    {"BIGINT", SQL_BIGINT, 19, "", "", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "BIGINT", 0, 0, SQL_BIGINT, 0, 10, 0},
    {"DECIMAL", SQL_DECIMAL, 38, "", "", "precision,scale", SQL_NULLABLE, 0, 3, 0, 0, 0, "DECIMAL", 0, 9, SQL_DECIMAL, 0, 10, 0},
    {"NUMERIC", SQL_NUMERIC, 38, "", "", "precision,scale", SQL_NULLABLE, 0, 3, 0, 0, 0, "NUMERIC", 0, 9, SQL_NUMERIC, 0, 10, 0},
    {"MONEY", SQL_DECIMAL, 19, "", "", "", SQL_NULLABLE, 0, 3, 0, 1, 0, "MONEY", 2, 2, SQL_DECIMAL, 0, 10, 0},
    {"REAL", SQL_REAL, 7, "", "", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "REAL", 0, 0, SQL_REAL, 0, 2, 0},
    {"FLOAT", SQL_FLOAT, 15, "", "", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "FLOAT", 0, 0, SQL_FLOAT, 0, 2, 0},
    {"DOUBLE", SQL_DOUBLE, 15, "", "", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "DOUBLE", 0, 0, SQL_DOUBLE, 0, 2, 0},
    {"INET", SQL_VARCHAR, 64, "'", "'", "", SQL_NULLABLE, 1, 3, 0, 0, 0, "INET", 0, 0, SQL_VARCHAR, 0, 0, 0},
    {"CIDR", SQL_VARCHAR, 64, "'", "'", "", SQL_NULLABLE, 1, 3, 0, 0, 0, "CIDR", 0, 0, SQL_VARCHAR, 0, 0, 0},
    {"MACADDR", SQL_VARCHAR, 32, "'", "'", "", SQL_NULLABLE, 1, 3, 0, 0, 0, "MACADDR", 0, 0, SQL_VARCHAR, 0, 0, 0},
    {"MACADDR8", SQL_VARCHAR, 32, "'", "'", "", SQL_NULLABLE, 1, 3, 0, 0, 0, "MACADDR8", 0, 0, SQL_VARCHAR, 0, 0, 0},
    {"DATE", SQL_TYPE_DATE, 10, "'", "'", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "DATE", 0, 0, SQL_TYPE_DATE, 0, 0, 0},
    {"TIME", SQL_TYPE_TIME, 8, "'", "'", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "TIME", 0, 0, SQL_TYPE_TIME, 0, 0, 0},
    {"TIMESTAMP", SQL_TYPE_TIMESTAMP, 26, "'", "'", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "TIMESTAMP", 0, 6, SQL_TYPE_TIMESTAMP, 0, 0, 0},
    {"TIMESTAMPTZ", SQL_TYPE_TIMESTAMP, 35, "'", "'", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "TIMESTAMPTZ", 0, 6, SQL_TYPE_TIMESTAMP, 0, 0, 0},
    {"INTERVAL", SQL_VARCHAR, 64, "'", "'", "", SQL_NULLABLE, 1, 3, 0, 0, 0, "INTERVAL", 0, 0, SQL_VARCHAR, 0, 0, 0},
    {"UUID", SQL_GUID, 36, "'", "'", "", SQL_NULLABLE, 0, 3, 0, 0, 0, "UUID", 0, 0, SQL_GUID, 0, 0, 0},
};

std::string sqlTypeName(SQLSMALLINT type) {
    switch (type) {
        case SQL_CHAR: return "CHAR";
        case SQL_VARCHAR: return "VARCHAR";
        case SQL_LONGVARCHAR: return "LONGVARCHAR";
        case SQL_TINYINT: return "TINYINT";
        case SQL_SMALLINT: return "SMALLINT";
        case SQL_INTEGER: return "INTEGER";
        case SQL_BIGINT: return "BIGINT";
        case SQL_DECIMAL: return "DECIMAL";
        case SQL_NUMERIC: return "NUMERIC";
        case SQL_REAL: return "REAL";
        case SQL_DOUBLE: return "DOUBLE";
        case SQL_BIT: return "BIT";
        case SQL_BINARY: return "BINARY";
        case SQL_VARBINARY: return "VARBINARY";
        case SQL_LONGVARBINARY: return "LONGVARBINARY";
        case SQL_TYPE_DATE: return "DATE";
        case SQL_TYPE_TIME: return "TIME";
        case SQL_TYPE_TIMESTAMP: return "TIMESTAMP";
        case SQL_GUID: return "GUID";
        default: return "UNKNOWN";
    }
}

ColumnMetadata makeCatalogColumn(const std::string& name, SQLSMALLINT type, SQLULEN size = 0) {
    ColumnMetadata meta;
    meta.name = name;
    meta.sql_type = type;
    meta.type_name = sqlTypeName(type);
    meta.column_size = size;
    meta.nullable = SQL_NULLABLE;
    return meta;
}
} // namespace

bool supportsPreparedTransactions() {
    return true;
}

bool supportsDormantReattach() {
    return false;
}

bool supportsPortalResume() {
    return false;
}

SQLRETURN buildPreparedTransactionSql(const std::string& verb,
                                      const std::string& global_transaction_id,
                                      std::string& out_sql,
                                      std::string* sqlstate_out,
                                      std::string* message_out) {
    const std::string trimmed_verb = trimString(verb);
    const std::string trimmed_gid = trimString(global_transaction_id);

    out_sql.clear();
    if (sqlstate_out) {
        sqlstate_out->clear();
    }
    if (message_out) {
        message_out->clear();
    }

    if (trimmed_verb.empty()) {
        if (sqlstate_out) {
            *sqlstate_out = "HY024";
        }
        if (message_out) {
            *message_out = "Prepared-transaction verb is required";
        }
        return SQL_ERROR;
    }

    if (trimmed_gid.empty()) {
        if (sqlstate_out) {
            *sqlstate_out = "42601";
        }
        if (message_out) {
            *message_out = "Global transaction id is required";
        }
        return SQL_ERROR;
    }

    out_sql = trimString(trimmed_verb) + " " + quoteSqlLiteral(trimmed_gid);
    return SQL_SUCCESS;
}

SQLRETURN rejectDormantReattach(const char* operation,
                                std::string* sqlstate_out,
                                std::string* message_out) {
    if (sqlstate_out) {
        *sqlstate_out = "0A000";
    }
    if (message_out) {
        std::string action = operation ? trimString(operation) : std::string();
        if (action.empty()) {
            action = "reattach";
        }
        *message_out = "dormant " + action + " is not exposed by the ODBC front door";
    }
    return SQL_ERROR;
}

// =============================================================================
// OdbcHandle Base Implementation
// =============================================================================

void OdbcHandle::addDiagnostic(const DiagnosticRecord& record) {
    std::lock_guard lock(diagnostics_mutex_);
    diagnostics_.push_back(record);
}

void OdbcHandle::clearDiagnostics() {
    std::lock_guard lock(diagnostics_mutex_);
    diagnostics_.clear();
}

SQLSMALLINT OdbcHandle::getDiagnosticCount() const {
    std::lock_guard lock(diagnostics_mutex_);
    return static_cast<SQLSMALLINT>(diagnostics_.size());
}

const DiagnosticRecord* OdbcHandle::getDiagnostic(SQLSMALLINT rec_number) const {
    std::lock_guard lock(diagnostics_mutex_);
    if (rec_number < 1 || static_cast<size_t>(rec_number) > diagnostics_.size()) {
        return nullptr;
    }
    return &diagnostics_[rec_number - 1];
}

void OdbcHandle::setError(const std::string& sqlstate, SQLINTEGER native_error,
                          const std::string& message) {
    DiagnosticRecord rec;
    rec.sqlstate = sqlstate;
    rec.native_error = native_error;
    rec.message = message;
    addDiagnostic(rec);
}

// =============================================================================
// OdbcEnvironment Implementation
// =============================================================================

OdbcEnvironment::OdbcEnvironment() {
    keepalive_manager_.Start();
    leak_detector_.Start();
}

OdbcEnvironment::~OdbcEnvironment() {
    keepalive_manager_.Stop();
    leak_detector_.Stop();
    std::lock_guard lock(connections_mutex_);
    connections_.clear();
}

SQLRETURN OdbcEnvironment::setAttribute(SQLINTEGER attribute, SQLPOINTER value,
                                         SQLINTEGER /*string_length*/) {
    clearDiagnostics();

    switch (attribute) {
        case SQL_ATTR_ODBC_VERSION:
            odbc_version_ = ODBC_PTR_TO_UINT(value);
            if (odbc_version_ != SQL_OV_ODBC2 &&
                odbc_version_ != SQL_OV_ODBC3 &&
                odbc_version_ != SQL_OV_ODBC3_80) {
                setError("HY024", 0, "Invalid attribute value");
                return SQL_ERROR;
            }
            break;

        case SQL_ATTR_CONNECTION_POOLING:
            connection_pooling_ = ODBC_PTR_TO_UINT(value);
            break;

        case SQL_ATTR_CP_MATCH:
            cp_match_ = ODBC_PTR_TO_UINT(value);
            break;

        case SQL_ATTR_OUTPUT_NTS:
            output_nts_ = (ODBC_PTR_TO_UINT(value) != 0);
            break;

        default:
            setError("HY092", 0, "Invalid attribute identifier");
            return SQL_ERROR;
    }

    return SQL_SUCCESS;
}

SQLRETURN OdbcEnvironment::getAttribute(SQLINTEGER attribute, SQLPOINTER value,
                                         SQLINTEGER buffer_length,
                                         SQLINTEGER* string_length) {
    clearDiagnostics();

    switch (attribute) {
        case SQL_ATTR_ODBC_VERSION:
            if (value) {
                *static_cast<SQLUINTEGER*>(value) = odbc_version_;
            }
            if (string_length) {
                *string_length = sizeof(SQLUINTEGER);
            }
            break;

        case SQL_ATTR_CONNECTION_POOLING:
            if (value) {
                *static_cast<SQLUINTEGER*>(value) = connection_pooling_;
            }
            if (string_length) {
                *string_length = sizeof(SQLUINTEGER);
            }
            break;

        case SQL_ATTR_CP_MATCH:
            if (value) {
                *static_cast<SQLUINTEGER*>(value) = cp_match_;
            }
            if (string_length) {
                *string_length = sizeof(SQLUINTEGER);
            }
            break;

        case SQL_ATTR_OUTPUT_NTS:
            if (value) {
                *static_cast<SQLUINTEGER*>(value) = output_nts_ ? 1 : 0;
            }
            if (string_length) {
                *string_length = sizeof(SQLUINTEGER);
            }
            break;

        default:
            setError("HY092", 0, "Invalid attribute identifier");
            return SQL_ERROR;
    }

    (void)buffer_length;  // Not used for these attributes
    return SQL_SUCCESS;
}

OdbcConnection* OdbcEnvironment::createConnection() {
    std::lock_guard lock(connections_mutex_);
    auto conn = std::make_unique<OdbcConnection>(this);
    auto* ptr = conn.get();
    connections_.push_back(std::move(conn));
    return ptr;
}

void OdbcEnvironment::removeConnection(OdbcConnection* conn) {
    std::lock_guard lock(connections_mutex_);
    connections_.erase(
        std::remove_if(connections_.begin(), connections_.end(),
                       [conn](const auto& c) { return c.get() == conn; }),
        connections_.end());
}

size_t OdbcEnvironment::getConnectionCount() const {
    std::lock_guard lock(connections_mutex_);
    return connections_.size();
}

SQLRETURN OdbcEnvironment::endTransaction(SQLSMALLINT completion_type) {
    clearDiagnostics();

    if (completion_type != SQL_COMMIT && completion_type != SQL_ROLLBACK) {
        setError("HY012", 0, "Invalid transaction operation code");
        return SQL_ERROR;
    }

    std::vector<OdbcConnection*> connections;
    {
        std::lock_guard lock(connections_mutex_);
        connections.reserve(connections_.size());
        for (const auto& conn : connections_) {
            connections.push_back(conn.get());
        }
    }

    SQLRETURN overall = SQL_SUCCESS;
    for (auto* conn : connections) {
        if (!conn || !conn->isConnected()) {
            continue;
        }

        auto status = conn->endTransaction(completion_type);
        if (status == SQL_SUCCESS_WITH_INFO) {
            overall = SQL_SUCCESS_WITH_INFO;
            continue;
        }

        if (status != SQL_SUCCESS) {
            const auto* diag = conn->getDiagnostic(1);
            if (diag) {
                setError(diag->sqlstate, diag->native_error, diag->message);
            } else {
                setError("HY000", 0, "Failed to complete transaction for one or more connections");
            }
            return SQL_ERROR;
        }
    }

    return overall;
}

// =============================================================================
// OdbcConnection Implementation
// =============================================================================

OdbcConnection::OdbcConnection(OdbcEnvironment* env)
    : env_(env),
      client_bridge_(std::make_unique<OdbcClientBridge>()) {
    connection_id_ = "conn-" + std::to_string(kConnectionIdCounter.fetch_add(1) + 1);
}

OdbcConnection::~OdbcConnection() {
    if (connected_) {
        disconnect();
    }
}

SQLRETURN OdbcConnection::connect(const SQLCHAR* dsn, SQLSMALLINT dsn_len,
                                   const SQLCHAR* user, SQLSMALLINT user_len,
                                   const SQLCHAR* password, SQLSMALLINT password_len) {
    clearDiagnostics();

    if (connected_) {
        setError("08002", 0, "Connection already open");
        return SQL_ERROR;
    }

    // Extract DSN name
    std::string dsn_str;
    if (dsn) {
        dsn_str = (dsn_len == SQL_NTS) ?
            std::string(reinterpret_cast<const char*>(dsn)) :
            std::string(reinterpret_cast<const char*>(dsn), dsn_len);
    }

    // Extract user
    if (user) {
        params_.user = (user_len == SQL_NTS) ?
            std::string(reinterpret_cast<const char*>(user)) :
            std::string(reinterpret_cast<const char*>(user), user_len);
    }

    // Extract password
    if (password) {
        params_.password = (password_len == SQL_NTS) ?
            std::string(reinterpret_cast<const char*>(password)) :
            std::string(reinterpret_cast<const char*>(password), password_len);
    }

    auto dsn_result = applyDsnConfig(dsn_str);
    if (dsn_result != SQL_SUCCESS) {
        return dsn_result;
    }

    return establishConnection();
}

SQLRETURN OdbcConnection::driverConnect(HWND /*window_handle*/,
                                         const SQLCHAR* conn_str, SQLSMALLINT conn_str_len,
                                         SQLCHAR* out_conn_str, SQLSMALLINT out_buffer_len,
                                         SQLSMALLINT* out_conn_str_len,
                                         SQLUSMALLINT /*driver_completion*/) {
    clearDiagnostics();

    if (connected_) {
        setError("08002", 0, "Connection already open");
        return SQL_ERROR;
    }

    // Parse connection string
    std::string conn_str_s;
    if (conn_str) {
        conn_str_s = (conn_str_len == SQL_NTS) ?
            std::string(reinterpret_cast<const char*>(conn_str)) :
            std::string(reinterpret_cast<const char*>(conn_str), conn_str_len);
    }

    auto result = parseConnectionString(conn_str_s);
    if (result != SQL_SUCCESS) {
        return result;
    }

    result = establishConnection();
    if (result != SQL_SUCCESS) {
        return result;
    }

    // Build output connection string
    std::string out_str = buildConnectionString();
    if (out_conn_str && out_buffer_len > 0) {
        size_t copy_len = std::min(static_cast<size_t>(out_buffer_len - 1), out_str.size());
        std::memcpy(out_conn_str, out_str.c_str(), copy_len);
        out_conn_str[copy_len] = '\0';
        if (out_str.size() >= static_cast<size_t>(out_buffer_len)) {
            setError("01004", 0, "String data, right truncated");
            result = SQL_SUCCESS_WITH_INFO;
        }
    }
    if (out_conn_str_len) {
        *out_conn_str_len = static_cast<SQLSMALLINT>(out_str.size());
    }

    return result;
}

SQLRETURN OdbcConnection::browseConnect(const SQLCHAR* in_conn_str, SQLSMALLINT in_conn_str_len,
                                         SQLCHAR* out_conn_str, SQLSMALLINT out_buffer_len,
                                         SQLSMALLINT* out_conn_str_len) {
    clearDiagnostics();

    std::string input;
    if (in_conn_str) {
        input = (in_conn_str_len == SQL_NTS) ?
            std::string(reinterpret_cast<const char*>(in_conn_str)) :
            std::string(reinterpret_cast<const char*>(in_conn_str), in_conn_str_len);
    }

    std::map<std::string, std::string> options;
    parseBrowseInput(input, in_conn_str_len, options);
    BrowseStage stage = parseBrowseStage(options);

    SQLRETURN result = SQL_SUCCESS;
    std::string out;
    appendPersistentConnectionAttributes(options, out);

    auto emitResponse = [&](bool need_data) {
        return copyBrowseResponse(out, out_conn_str, out_buffer_len, out_conn_str_len,
                                  need_data, result, this);
    };

    auto appendField = [&](const char* key, const std::string& value) {
        appendBrowseField(out, key, value);
    };

    auto add_dsn = [&](const std::string& dsn) {
        appendField(kMetaBrowseDsnKey, dsn);
    };

    auto add_catalog = [&](const std::string& catalog) {
        appendField(kMetaBrowseCatalogKey, catalog);
    };

    auto add_schema = [&](const std::string& schema) {
        appendField(kMetaBrowseSchemaKey, schema);
    };

    auto add_table = [&](const std::string& table) {
        appendField(kMetaBrowseTableKey, table);
    };

    auto add_column = [&](const std::string& column) {
        appendField(kMetaBrowseColumnKey, column);
    };

    auto appendContext = [&]() {
        if (stage.has_dsn) {
            add_dsn(stage.dsn);
        }
        if (stage.has_catalog) {
            add_catalog(stage.catalog);
        }
        if (stage.has_schema) {
            add_schema(stage.schema);
        }
        if (stage.has_table) {
            add_table(stage.table);
        }
        if (stage.has_column) {
            add_column(stage.column);
        }
    };

    auto ensureConnection = [&]() -> SQLRETURN {
        if (connected_ && !params_.dsn.empty() && toLower(params_.dsn) == toLower(stage.dsn)) {
            return SQL_SUCCESS;
        }
        auto parse_result = parseConnectionString(input);
        if (parse_result != SQL_SUCCESS) {
            if (!stage.has_dsn) {
                return parse_result;
            }
            const std::string dsn_conn = std::string(kMetaBrowseDsnKey) + "=" + stage.dsn + ";";
            auto fallback_parse_result = parseConnectionString(dsn_conn);
            if (fallback_parse_result != SQL_SUCCESS) {
                return parse_result;
            }
        }
        return establishConnection();
    };

    auto executeMetadataQuery = [&](const std::string& sql,
                                   const std::string& filter_value,
                                   int filter_col,
                                   std::vector<std::vector<std::string>>* rows) -> SQLRETURN {
        if (sql.empty()) {
            setError("HY000", 0, "Invalid metadata query");
            return SQL_ERROR;
        }

        SQLLEN rows_affected = 0;
        std::vector<ColumnMetadata> cols;
        rows->clear();
        auto status = executeSQL(sql, *rows, cols, rows_affected);
        if (status != SQL_SUCCESS) {
            return status;
        }
        if (filter_value.empty() || filter_col < 0) {
            return SQL_SUCCESS;
        }

        std::vector<std::vector<std::string>> filtered;
        filtered.reserve(rows->size());
        for (const auto& row : *rows) {
            if (filter_col >= static_cast<int>(row.size()) || row[filter_col].empty()) {
                continue;
            }
            if (!matchPattern(row[filter_col], filter_value, false)) {
                continue;
            }
            filtered.push_back(row);
        }
        rows->swap(filtered);
        return SQL_SUCCESS;
    };

    auto filterMetadataRows = [&](const std::string& filter_value,
                                 int filter_col,
                                 std::vector<std::vector<std::string>>* rows_to_filter) {
        if (filter_value.empty() || filter_col < 0) {
            return SQL_SUCCESS;
        }

        std::vector<std::vector<std::string>> filtered;
        filtered.reserve(rows_to_filter->size());
        for (const auto& row : *rows_to_filter) {
            if (filter_col >= static_cast<int>(row.size()) || row[filter_col].empty()) {
                continue;
            }
            if (!matchPattern(row[filter_col], filter_value, false)) {
                continue;
            }
            filtered.push_back(row);
        }
        rows_to_filter->swap(filtered);
        return SQL_SUCCESS;
    };

    if (!stage.has_dsn) {
        auto names = discoverIniDsns();
        if (names.empty()) {
            setError("IM002", 0, "No browse data source names available");
            return SQL_ERROR;
        }
        for (const auto& dsn : names) {
            add_dsn(dsn);
        }
        if (!emitResponse(false)) {
            return result;
        }
        return result;
    }

    if (connected_ && toLower(params_.dsn) != toLower(stage.dsn) && !stage.dsn.empty()) {
        auto ensure_result = ensureConnection();
        if (ensure_result != SQL_SUCCESS) {
            return ensure_result;
        }
    } else if (!connected_ && !stage.dsn.empty()) {
        auto ensure_result = ensureConnection();
        if (ensure_result != SQL_SUCCESS) {
            return ensure_result;
        }
    }

    std::vector<std::vector<std::string>> rows;
    if (!stage.has_catalog && !stage.has_schema && !stage.has_table && !stage.has_column) {
        result = executeMetadataQuery("SHOW DATABASES", {}, 0, &rows);
        if (result != SQL_SUCCESS) {
            return result;
        }
        appendContext();
        for (const auto& row : rows) {
            if (!row.empty() && !row[0].empty()) {
                add_catalog(row[0]);
            }
        }
        emitResponse(true);
        return result;
    }

    if (stage.has_catalog && !stage.has_schema) {
        result = executeMetadataQuery(metadata::kSchemasQuery, {}, 0, &rows);
        if (result != SQL_SUCCESS) {
            return result;
        }
        std::vector<std::string> schema_paths;
        schema_paths.reserve(rows.size());
        for (const auto& row : rows) {
            if (!row.empty() && !row[0].empty()) {
                schema_paths.push_back(row[0]);
            }
        }
        const auto schema_tree =
            metadata::buildMetadataSchemaTree(schema_paths, stage.catalog, true);
        const auto top_level_schemas = metadata::metadataSchemaChildren(schema_tree, "");
        appendContext();
        for (const auto& schema_path : top_level_schemas) {
            add_schema(schema_path);
        }
        emitResponse(true);
        return result;
    }

    if (stage.has_schema && !stage.has_table) {
        std::vector<std::vector<std::string>> schema_rows;
        result = executeMetadataQuery(metadata::kSchemasQuery, {}, 0, &schema_rows);
        if (result != SQL_SUCCESS) {
            return result;
        }
        std::vector<std::string> schema_paths;
        schema_paths.reserve(schema_rows.size());
        for (const auto& row : schema_rows) {
            if (!row.empty() && !row[0].empty()) {
                schema_paths.push_back(row[0]);
            }
        }
        const std::string normalized_schema = metadata::normalizeSchemaPath(stage.schema);
        const std::string schema_filter = normalized_schema.empty() ? stage.schema : normalized_schema;
        const auto schema_tree =
            metadata::buildMetadataSchemaTree(schema_paths, stage.catalog, true);
        const auto child_schemas = metadata::metadataSchemaChildren(schema_tree, schema_filter);

        appendContext();
        if (!child_schemas.empty()) {
            for (const auto& child_schema : child_schemas) {
                add_schema(child_schema);
            }
            emitResponse(true);
            return result;
        }

        result = executeMetadataQuery(metadata::kTablesQuery, {}, 0, &rows);
        if (result != SQL_SUCCESS) {
            return result;
        }
        result = filterMetadataRows(schema_filter, 1, &rows);
        if (result != SQL_SUCCESS) {
            return result;
        }
        for (const auto& row : rows) {
            if (row.empty()) {
                continue;
            }
            add_table(row[0]);
        }
        emitResponse(true);
        return result;
    }

    if (stage.has_table && !stage.has_column) {
        result = executeMetadataQuery(metadata::kColumnsQuery, {}, 0, &rows);
        if (result != SQL_SUCCESS) {
            return result;
        }
        result = filterMetadataRows(stage.table, 1, &rows);
        if (result != SQL_SUCCESS) {
            return result;
        }
        result = filterMetadataRows(stage.schema, 2, &rows);
        if (result != SQL_SUCCESS) {
            return result;
        }
        appendContext();
        for (const auto& row : rows) {
            if (row.size() < 3 || row[0].empty()) {
                continue;
            }
            add_column(row[0]);
        }
        emitResponse(true);
        return result;
    }

    appendContext();
    emitResponse(false);
    return SQL_SUCCESS;
}

SQLRETURN OdbcConnection::disconnect() {
    clearDiagnostics();

    if (!connected_) {
        setError("08003", 0, "Connection not open");
        return SQL_ERROR;
    }

    unregisterResilience();

    // Close all statements
    {
        std::lock_guard lock(statements_mutex_);
        statements_.clear();
    }
    prepared_sql_.clear();

    if (client_bridge_) {
        client_bridge_->disconnect();
    }
    connected_ = false;
    connection_dead_ = false;
    in_transaction_ = false;

    return SQL_SUCCESS;
}

SQLRETURN OdbcConnection::setAttribute(SQLINTEGER attribute, SQLPOINTER value,
                                        SQLINTEGER string_length) {
    clearDiagnostics();

    switch (attribute) {
        case SQL_ATTR_ACCESS_MODE:
            access_mode_ = ODBC_PTR_TO_UINT(value);
            break;

        case SQL_ATTR_AUTOCOMMIT:
        {
            auto new_value = ODBC_PTR_TO_UINT(value);
            if (new_value != SQL_AUTOCOMMIT_ON && new_value != SQL_AUTOCOMMIT_OFF) {
                setError("HY024", 0, "Invalid attribute value");
                return SQL_ERROR;
            }
            auto_commit_ = new_value;
            if (connected_) {
                auto result = applyAutocommitSetting();
                if (result != SQL_SUCCESS && result != SQL_SUCCESS_WITH_INFO) {
                    return result;
                }
            }
            in_transaction_ = (auto_commit_ == SQL_AUTOCOMMIT_OFF);
            break;
        }

        case SQL_ATTR_LOGIN_TIMEOUT:
            login_timeout_ = ODBC_PTR_TO_UINT(value);
            break;

        case SQL_ATTR_CONNECTION_TIMEOUT:
            connection_timeout_ = ODBC_PTR_TO_UINT(value);
            break;

        case SQL_ATTR_TXN_ISOLATION:
        {
            auto new_value = ODBC_PTR_TO_UINT(value);
            std::string sql;
            if (!buildIsolationSql(new_value, sql)) {
                setError("HY024", 0, "Invalid attribute value");
                return SQL_ERROR;
            }
            txn_isolation_ = new_value;
            if (connected_) {
                auto result = applyIsolationSetting();
                if (result != SQL_SUCCESS && result != SQL_SUCCESS_WITH_INFO) {
                    return result;
                }
                if (auto_commit_ == SQL_AUTOCOMMIT_ON) {
                    result = applyAutocommitSetting();
                    if (result != SQL_SUCCESS && result != SQL_SUCCESS_WITH_INFO) {
                        return result;
                    }
                }
            }
            break;
        }

        case SQL_ATTR_CURRENT_CATALOG:
            if (value) {
                current_database_ = (string_length == SQL_NTS) ?
                    std::string(reinterpret_cast<const char*>(value)) :
                    std::string(reinterpret_cast<const char*>(value), string_length);
                if (connected_) {
                    std::string escaped;
                    escaped.reserve(current_database_.size() + 4);
                    for (char ch : current_database_) {
                        if (ch == '\'') {
                            escaped += "''";
                        } else {
                            escaped.push_back(ch);
                        }
                    }
                    std::string sql = "SET search_path TO '" + escaped + "'";
                    std::vector<std::vector<std::string>> results;
                    std::vector<ColumnMetadata> columns;
                    SQLLEN rows_affected = 0;
                    auto result = executeSQL(sql, results, columns, rows_affected);
                    if (result != SQL_SUCCESS && result != SQL_SUCCESS_WITH_INFO) {
                        return result;
                    }
                    current_schema_ = current_database_;
                }
            }
            break;

        case SQL_ATTR_PACKET_SIZE:
            if (!connected_) {
                packet_size_ = ODBC_PTR_TO_UINT(value);
            } else {
                setError("HY011", 0, "Attribute cannot be set now");
                return SQL_ERROR;
            }
            break;

        case SQL_ATTR_METADATA_ID:
            metadata_id_ = (ODBC_PTR_TO_UINT(value) != 0);
            break;

        case SQL_ATTR_TRACE:
        case SQL_ATTR_TRACEFILE:
        case SQL_ATTR_TRANSLATE_LIB:
        case SQL_ATTR_TRANSLATE_OPTION:
        case SQL_ATTR_QUIET_MODE:
        case SQL_ATTR_ODBC_CURSORS:
            // Handled by Driver Manager
            break;

        default:
            setError("HY092", 0, "Invalid attribute identifier");
            return SQL_ERROR;
    }

    return SQL_SUCCESS;
}

SQLRETURN OdbcConnection::getAttribute(SQLINTEGER attribute, SQLPOINTER value,
                                        SQLINTEGER buffer_length,
                                        SQLINTEGER* string_length) {
    clearDiagnostics();

    auto copyString = [&](const std::string& str) -> SQLRETURN {
        if (string_length) {
            *string_length = static_cast<SQLINTEGER>(str.size());
        }
        if (value && buffer_length > 0) {
            size_t copy_len = std::min(static_cast<size_t>(buffer_length - 1), str.size());
            std::memcpy(value, str.c_str(), copy_len);
            static_cast<char*>(value)[copy_len] = '\0';
            if (str.size() >= static_cast<size_t>(buffer_length)) {
                setError("01004", 0, "String data, right truncated");
                return SQL_SUCCESS_WITH_INFO;
            }
        }
        return SQL_SUCCESS;
    };

    switch (attribute) {
        case SQL_ATTR_ACCESS_MODE:
            if (value) *static_cast<SQLUINTEGER*>(value) = access_mode_;
            if (string_length) *string_length = sizeof(SQLUINTEGER);
            break;

        case SQL_ATTR_AUTOCOMMIT:
            if (value) *static_cast<SQLUINTEGER*>(value) = auto_commit_;
            if (string_length) *string_length = sizeof(SQLUINTEGER);
            break;

        case SQL_ATTR_LOGIN_TIMEOUT:
            if (value) *static_cast<SQLUINTEGER*>(value) = login_timeout_;
            if (string_length) *string_length = sizeof(SQLUINTEGER);
            break;

        case SQL_ATTR_CONNECTION_TIMEOUT:
            if (value) *static_cast<SQLUINTEGER*>(value) = connection_timeout_;
            if (string_length) *string_length = sizeof(SQLUINTEGER);
            break;

        case SQL_ATTR_TXN_ISOLATION:
            if (value) *static_cast<SQLUINTEGER*>(value) = txn_isolation_;
            if (string_length) *string_length = sizeof(SQLUINTEGER);
            break;

        case SQL_ATTR_CURRENT_CATALOG:
            return copyString(current_database_);

        case SQL_ATTR_PACKET_SIZE:
            if (value) *static_cast<SQLUINTEGER*>(value) = packet_size_;
            if (string_length) *string_length = sizeof(SQLUINTEGER);
            break;

        case SQL_ATTR_CONNECTION_DEAD:
            if (value) *static_cast<SQLUINTEGER*>(value) = connection_dead_ ? 1 : 0;
            if (string_length) *string_length = sizeof(SQLUINTEGER);
            break;

        case SQL_ATTR_METADATA_ID:
            if (value) *static_cast<SQLUINTEGER*>(value) = metadata_id_ ? 1 : 0;
            if (string_length) *string_length = sizeof(SQLUINTEGER);
            break;

        case SQL_ATTR_AUTO_IPD:
            if (value) *static_cast<SQLUINTEGER*>(value) = 1;  // We support auto IPD
            if (string_length) *string_length = sizeof(SQLUINTEGER);
            break;

        default:
            setError("HY092", 0, "Invalid attribute identifier");
            return SQL_ERROR;
    }

    return SQL_SUCCESS;
}

SQLRETURN OdbcConnection::endTransaction(SQLSMALLINT completion_type) {
    clearDiagnostics();

    if (!connected_) {
        setError("08003", 0, "Connection not open");
        return SQL_ERROR;
    }
    if (!allowRequest()) {
        return SQL_ERROR;
    }

    if (auto_commit_ == SQL_AUTOCOMMIT_ON && !in_transaction_) {
        // No explicit transaction to commit/rollback
        return SQL_SUCCESS;
    }

    if (completion_type == SQL_COMMIT) {
        auto result = client_bridge_ ? client_bridge_->commit() : SQL_ERROR;
        if (result == SQL_SUCCESS) {
            in_transaction_ = (auto_commit_ == SQL_AUTOCOMMIT_OFF);
            recordSuccess();
        } else if (client_bridge_) {
            auto status = client_bridge_->lastStatus();
            auto message = client_bridge_->lastError();
            setError(mapStatusToSqlState(status), static_cast<SQLINTEGER>(status),
                     message.empty() ? "Commit failed" : message);
            recordFailure();
        }
        return result;
    } else if (completion_type == SQL_ROLLBACK) {
        auto result = client_bridge_ ? client_bridge_->rollback() : SQL_ERROR;
        if (result == SQL_SUCCESS) {
            in_transaction_ = (auto_commit_ == SQL_AUTOCOMMIT_OFF);
            recordSuccess();
        } else if (client_bridge_) {
            auto status = client_bridge_->lastStatus();
            auto message = client_bridge_->lastError();
            setError(mapStatusToSqlState(status), static_cast<SQLINTEGER>(status),
                     message.empty() ? "Rollback failed" : message);
            recordFailure();
        }
        return result;
    } else {
        setError("HY012", 0, "Invalid transaction operation code");
        recordFailure();
        return SQL_ERROR;
    }
}

SQLRETURN OdbcConnection::beginTransaction() {
    clearDiagnostics();

    if (!connected_) {
        setError("08003", 0, "Connection not open");
        return SQL_ERROR;
    }
    if (!allowRequest()) {
        return SQL_ERROR;
    }

    if (in_transaction_) {
        return SQL_SUCCESS;  // Already in transaction
    }

    auto result = client_bridge_ ? client_bridge_->beginTransaction() : SQL_ERROR;
    if (result == SQL_SUCCESS) {
        in_transaction_ = true;
        recordSuccess();
    } else if (client_bridge_) {
        auto status = client_bridge_->lastStatus();
        auto message = client_bridge_->lastError();
        setError(mapStatusToSqlState(status), static_cast<SQLINTEGER>(status),
                 message.empty() ? "Begin transaction failed" : message);
        recordFailure();
    }

    return result;
}

SQLRETURN OdbcConnection::getInfo(SQLUSMALLINT info_type, SQLPOINTER info_value,
                                   SQLSMALLINT buffer_length, SQLSMALLINT* string_length) {
    clearDiagnostics();

    auto copyString = [&](const char* str) -> SQLRETURN {
        size_t len = std::strlen(str);
        if (string_length) {
            *string_length = static_cast<SQLSMALLINT>(len);
        }
        if (info_value && buffer_length > 0) {
            size_t copy_len = std::min(static_cast<size_t>(buffer_length - 1), len);
            std::memcpy(info_value, str, copy_len);
            static_cast<char*>(info_value)[copy_len] = '\0';
            if (len >= static_cast<size_t>(buffer_length)) {
                setError("01004", 0, "String data, right truncated");
                return SQL_SUCCESS_WITH_INFO;
            }
        }
        return SQL_SUCCESS;
    };

    auto setUSmallInt = [&](SQLUSMALLINT val) {
        if (info_value) *static_cast<SQLUSMALLINT*>(info_value) = val;
        if (string_length) *string_length = sizeof(SQLUSMALLINT);
    };

    auto setUInteger = [&](SQLUINTEGER val) {
        if (info_value) *static_cast<SQLUINTEGER*>(info_value) = val;
        if (string_length) *string_length = sizeof(SQLUINTEGER);
    };

    switch (info_type) {
        // Driver Information
        case SQL_DRIVER_NAME:
            return copyString(DriverConfig::DRIVER_NAME);
        case SQL_DRIVER_VER:
            return copyString(DriverConfig::DRIVER_VERSION);
        case SQL_DRIVER_ODBC_VER:
            return copyString(DriverConfig::ODBC_VERSION);
        case SQL_ODBC_VER:
            return copyString(DriverConfig::ODBC_VERSION);

        // DBMS Information
        case SQL_DBMS_NAME:
            return copyString(DriverConfig::DBMS_NAME);
        case SQL_DBMS_VER:
            return copyString(DriverConfig::DBMS_VERSION);
        case SQL_DATABASE_NAME:
            return copyString(current_database_.c_str());
        case SQL_SERVER_NAME:
            return copyString(params_.server.c_str());
        case SQL_USER_NAME:
            return copyString(current_user_.c_str());
        case SQL_DATA_SOURCE_NAME:
            return copyString(params_.dsn.c_str());

        // Capabilities
        case SQL_DATA_SOURCE_READ_ONLY:
            return copyString(access_mode_ == SQL_MODE_READ_ONLY ? "Y" : "N");
        case SQL_ACCESSIBLE_TABLES:
            return copyString("Y");
        case SQL_ACCESSIBLE_PROCEDURES:
            return copyString("Y");
        case SQL_MULT_RESULT_SETS:
            return copyString("N");
        case SQL_MULTIPLE_ACTIVE_TXN:
            return copyString("N");
        case SQL_PROCEDURES:
            return copyString("Y");
        case SQL_CATALOG_NAME:
            return copyString("Y");
        case SQL_COLUMN_ALIAS:
            return copyString("Y");
        case SQL_LIKE_ESCAPE_CLAUSE:
            return copyString("Y");
        case SQL_ORDER_BY_COLUMNS_IN_SELECT:
            return copyString("N");
        case SQL_OUTER_JOINS:
            return copyString("Y");
        case SQL_ROW_UPDATES:
            return copyString("N");
        case SQL_EXPRESSIONS_IN_ORDERBY:
            return copyString("Y");
        case SQL_INTEGRITY:
            return copyString("Y");

        // Identifier info
        case SQL_IDENTIFIER_QUOTE_CHAR:
            return copyString("\"");
        case SQL_CATALOG_NAME_SEPARATOR:
            return copyString(".");
        case SQL_CATALOG_TERM:
            return copyString("database");
        case SQL_SCHEMA_TERM:
            return copyString("schema");
        case SQL_TABLE_TERM:
            return copyString("table");
        case SQL_PROCEDURE_TERM:
            return copyString("function");
        case SQL_SEARCH_PATTERN_ESCAPE:
            return copyString("\\");
        case SQL_SPECIAL_CHARACTERS:
            return copyString("_");

        // Limits
        case SQL_MAX_CATALOG_NAME_LEN:
            setUSmallInt(DriverConfig::MAX_CATALOG_NAME_LEN);
            break;
        case SQL_MAX_SCHEMA_NAME_LEN:
            setUSmallInt(DriverConfig::MAX_SCHEMA_NAME_LEN);
            break;
        case SQL_MAX_TABLE_NAME_LEN:
            setUSmallInt(DriverConfig::MAX_TABLE_NAME_LEN);
            break;
        case SQL_MAX_COLUMN_NAME_LEN:
            setUSmallInt(DriverConfig::MAX_COLUMN_NAME_LEN);
            break;
        case SQL_MAX_COLUMNS_IN_INDEX:
            setUSmallInt(DriverConfig::MAX_COLUMNS_IN_INDEX);
            break;
        case SQL_MAX_COLUMNS_IN_TABLE:
            setUSmallInt(DriverConfig::MAX_COLUMNS_IN_TABLE);
            break;
        case SQL_MAX_STATEMENT_LEN:
            setUInteger(0);  // Unlimited
            break;
        case SQL_MAX_CONCURRENT_ACTIVITIES:
            setUSmallInt(0);  // Unlimited
            break;
        case SQL_MAX_DRIVER_CONNECTIONS:
            setUSmallInt(0);  // Unlimited
            break;
        case SQL_MAX_IDENTIFIER_LEN:
            setUSmallInt(128);
            break;

        // Transaction support
        case SQL_TXN_CAPABLE:
            setUSmallInt(2);  // SQL_TC_ALL
            break;
        case SQL_TXN_ISOLATION_OPTION:
            setUInteger(SQL_TXN_READ_UNCOMMITTED | SQL_TXN_READ_COMMITTED |
                       SQL_TXN_REPEATABLE_READ | SQL_TXN_SERIALIZABLE);
            break;
        case SQL_DEFAULT_TXN_ISOLATION:
            setUInteger(SQL_TXN_READ_COMMITTED);
            break;

        // SQL Conformance
        case SQL_SQL_CONFORMANCE:
            setUInteger(kSqlConformanceEntry);
            break;
        case SQL_ODBC_API_CONFORMANCE:
            setUSmallInt(kOdbcApiLevel1);
            break;
        case SQL_ODBC_SQL_CONFORMANCE:
            setUSmallInt(kOdbcSqlCore);
            break;

        // Identifier case
        case SQL_IDENTIFIER_CASE:
            setUSmallInt(2);  // SQL_IC_LOWER
            break;
        case SQL_QUOTED_IDENTIFIER_CASE:
            setUSmallInt(3);  // SQL_IC_SENSITIVE
            break;

        // Concatenation behavior
        case SQL_CONCAT_NULL_BEHAVIOR:
            setUSmallInt(0);  // SQL_CB_NULL
            break;

        // Correlation names
        case SQL_CORRELATION_NAME:
            setUSmallInt(2);  // SQL_CN_ANY
            break;

        // Group by
        case SQL_GROUP_BY:
            setUSmallInt(2);  // SQL_GB_GROUP_BY_CONTAINS_SELECT
            break;

        // Null collation
        case SQL_NULL_COLLATION:
            setUSmallInt(0);  // SQL_NC_HIGH
            break;

        // Cursor behavior
        case SQL_CURSOR_COMMIT_BEHAVIOR:
            setUSmallInt(0);  // SQL_CB_DELETE
            break;
        case SQL_CURSOR_ROLLBACK_BEHAVIOR:
            setUSmallInt(0);  // SQL_CB_DELETE
            break;

        // Non-nullable columns
        case SQL_NON_NULLABLE_COLUMNS:
            setUSmallInt(1);  // SQL_NNC_NON_NULL
            break;

        // Need long data length
        case SQL_NEED_LONG_DATA_LEN:
            return copyString("N");

        // Additional capability/feature probes frequently used by BI and driver managers.
        case SQL_ASYNC_MODE:
            setUInteger(0);  // SQL_AM_NONE
            break;
        case SQL_BATCH_ROW_COUNT:
            setUInteger(0);
            break;
        case SQL_BATCH_SUPPORT:
            setUInteger(0);
            break;
        case SQL_CATALOG_LOCATION:
            setUSmallInt(1);  // SQL_CL_START
            break;
        case SQL_CATALOG_USAGE:
            setUInteger(0);
            break;
        case SQL_SCHEMA_USAGE:
            setUInteger(0);
            break;
        case SQL_COLLATION_SEQ:
            return copyString("");
        case SQL_AGGREGATE_FUNCTIONS:
        case SQL_NUMERIC_FUNCTIONS:
        case SQL_STRING_FUNCTIONS:
        case SQL_SYSTEM_FUNCTIONS:
        case SQL_TIMEDATE_FUNCTIONS:
        case SQL_TIMEDATE_ADD_INTERVALS:
        case SQL_TIMEDATE_DIFF_INTERVALS:
        case SQL_CONVERT_FUNCTIONS:
        case SQL_CONVERT_BIGINT:
        case SQL_CONVERT_BINARY:
        case SQL_CONVERT_BIT:
        case SQL_CONVERT_CHAR:
        case SQL_CONVERT_DATE:
        case SQL_CONVERT_DECIMAL:
        case SQL_CONVERT_DOUBLE:
        case SQL_CONVERT_FLOAT:
        case SQL_CONVERT_INTEGER:
        case SQL_CONVERT_LONGVARBINARY:
        case SQL_CONVERT_LONGVARCHAR:
        case SQL_CONVERT_NUMERIC:
        case SQL_CONVERT_REAL:
        case SQL_CONVERT_SMALLINT:
        case SQL_CONVERT_TIME:
        case SQL_CONVERT_TIMESTAMP:
        case SQL_CONVERT_TINYINT:
        case SQL_CONVERT_VARBINARY:
        case SQL_CONVERT_VARCHAR:
        case SQL_ALTER_DOMAIN:
        case SQL_ALTER_TABLE:
        case SQL_CREATE_ASSERTION:
        case SQL_CREATE_CHARACTER_SET:
        case SQL_CREATE_COLLATION:
        case SQL_CREATE_DOMAIN:
        case SQL_CREATE_SCHEMA:
        case SQL_CREATE_TABLE:
        case SQL_CREATE_TRANSLATION:
        case SQL_CREATE_VIEW:
        case SQL_DROP_ASSERTION:
        case SQL_DROP_CHARACTER_SET:
        case SQL_DROP_COLLATION:
        case SQL_DROP_DOMAIN:
        case SQL_DROP_SCHEMA:
        case SQL_DROP_TABLE:
        case SQL_DROP_TRANSLATION:
        case SQL_DROP_VIEW:
        case SQL_DATETIME_LITERALS:
        case SQL_DDL_INDEX:
        case SQL_OJ_CAPABILITIES:
        case SQL_SQL92_DATETIME_FUNCTIONS:
        case SQL_SQL92_FOREIGN_KEY_DELETE_RULE:
        case SQL_SQL92_FOREIGN_KEY_UPDATE_RULE:
        case SQL_SQL92_GRANT:
        case SQL_SQL92_NUMERIC_VALUE_FUNCTIONS:
        case SQL_SQL92_PREDICATES:
        case SQL_SQL92_RELATIONAL_JOIN_OPERATORS:
        case SQL_SQL92_REVOKE:
        case SQL_SQL92_ROW_VALUE_CONSTRUCTOR:
        case SQL_SQL92_STRING_FUNCTIONS:
        case SQL_SQL92_VALUE_EXPRESSIONS:
        case SQL_STANDARD_CLI_CONFORMANCE:
        case SQL_SUBQUERIES:
        case SQL_UNION:
        case SQL_INFO_SCHEMA_VIEWS:
        case SQL_INSERT_STATEMENT:
            setUInteger(0);
            break;
        case SQL_KEYWORDS:
            return copyString("");
        case SQL_XOPEN_CLI_YEAR:
            return copyString("1995");

        // Limit probes where 0 indicates "no fixed limit".
        case SQL_MAX_BINARY_LITERAL_LEN:
        case SQL_MAX_CHAR_LITERAL_LEN:
        case SQL_MAX_INDEX_SIZE:
        case SQL_MAX_ROW_SIZE:
        case SQL_MAX_ASYNC_CONCURRENT_STATEMENTS:
            setUInteger(0);
            break;
        case SQL_MAX_COLUMNS_IN_GROUP_BY:
        case SQL_MAX_COLUMNS_IN_ORDER_BY:
        case SQL_MAX_COLUMNS_IN_SELECT:
        case SQL_MAX_TABLES_IN_SELECT:
            setUSmallInt(0);
            break;
        case SQL_MAX_PROCEDURE_NAME_LEN:
        case SQL_MAX_USER_NAME_LEN:
            setUSmallInt(128);
            break;
        case SQL_MAX_ROW_SIZE_INCLUDES_LONG:
            return copyString("Y");
        case SQL_FILE_USAGE:
            setUSmallInt(0);
            break;
        case SQL_DRIVER_AWARE_POOLING_SUPPORTED:
            setUInteger(0);
            break;

        // Additional commonly probed capability flags.
        case SQL_BOOKMARK_PERSISTENCE:
            setUInteger(0);
            break;
        case SQL_SCROLL_OPTIONS:
            setUInteger(0);
            break;
        case SQL_POS_OPERATIONS:
            setUInteger(0);
            break;
        case SQL_POSITIONED_STATEMENTS:
            setUInteger(0);
            break;
        case SQL_DYNAMIC_CURSOR_ATTRIBUTES1:
        case SQL_DYNAMIC_CURSOR_ATTRIBUTES2:
        case SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1:
        case SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2:
        case SQL_KEYSET_CURSOR_ATTRIBUTES1:
        case SQL_KEYSET_CURSOR_ATTRIBUTES2:
        case SQL_STATIC_CURSOR_ATTRIBUTES1:
        case SQL_STATIC_CURSOR_ATTRIBUTES2:
            setUInteger(0);
            break;
        case SQL_GETDATA_EXTENSIONS:
            setUInteger(0);
            break;
        case SQL_PARAM_ARRAY_ROW_COUNTS:
            setUSmallInt(0);
            break;
        case SQL_PARAM_ARRAY_SELECTS:
            setUSmallInt(0);
            break;
        case SQL_CURSOR_SENSITIVITY_VAL:
            setUInteger(SQL_UNSPECIFIED);
            break;
        case SQL_MAX_CURSOR_NAME_LEN:
            setUSmallInt(128);
            break;
        case SQL_DESCRIBE_PARAMETER:
            return copyString("Y");
        case SQL_ODBC_INTERFACE_CONFORMANCE:
            setUSmallInt(1);
            break;
        case SQL_ACTIVE_ENVIRONMENTS:
            setUSmallInt(1);
            break;
        case SQL_DM_VER:
            return copyString("03.80.0000");

        default:
            setError("HY096", 0, "Information type out of range");
            return SQL_ERROR;
    }

    return SQL_SUCCESS;
}

SQLRETURN OdbcConnection::getFunctions(SQLUSMALLINT function_id, SQLUSMALLINT* supported) {
    clearDiagnostics();

    if (!supported) {
        setError("HY009", 0, "Invalid use of null pointer");
        return SQL_ERROR;
    }

    // All ODBC 3.x core functions are supported
    static const SQLUSMALLINT supported_functions[] = {
        // Allocation and handle management
        SQL_API_SQLALLOCCONNECT,
        SQL_API_SQLALLOCENV,
        SQL_API_SQLALLOCSTMT,
        SQL_API_SQLALLOCHANDLE,
        SQL_API_SQLFREECONNECT,
        SQL_API_SQLFREEENV,
        SQL_API_SQLFREESTMT,
        SQL_API_SQLFREEHANDLE,
        SQL_API_SQLENDTRAN,

        // Connection and transaction management
        SQL_API_SQLCONNECT,
        SQL_API_SQLDRIVERCONNECT,
        SQL_API_SQLBROWSECONNECT,
        SQL_API_SQLDISCONNECT,
        SQL_API_SQLSETCONNECTATTR,
        SQL_API_SQLGETCONNECTATTR,
        SQL_API_SQLSETCONNECTOPTION,
        SQL_API_SQLGETCONNECTOPTION,
        SQL_API_SQLSETENVATTR,
        SQL_API_SQLGETENVATTR,

        // Statement management and execution
        SQL_API_SQLSETSTMTATTR,
        SQL_API_SQLGETSTMTATTR,
        SQL_API_SQLSETSTMTOPTION,
        SQL_API_SQLGETSTMTOPTION,
        SQL_API_SQLPREPARE,
        SQL_API_SQLEXECUTE,
        SQL_API_SQLEXECDIRECT,
        SQL_API_SQLCANCEL,
        SQL_API_SQLCANCELHANDLE,
        SQL_API_SQLCLOSECURSOR,
        SQL_API_SQLGETCURSORNAME,
        SQL_API_SQLSETCURSORNAME,
        SQL_API_SQLNATIVESQL,
        SQL_API_SQLBULKOPERATIONS,
        SQL_API_SQLSETPOS,
        SQL_API_SQLFETCH,
        SQL_API_SQLFETCHSCROLL,
        SQL_API_SQLMORERESULTS,

        // Parameter and column binding
        SQL_API_SQLBINDCOL,
        SQL_API_SQLBINDPARAM,
        SQL_API_SQLBINDPARAMETER,
        SQL_API_SQLNUMPARAMS,
        SQL_API_SQLDESCRIBEPARAM,
        SQL_API_SQLDESCRIBECOL,
        SQL_API_SQLNUMRESULTCOLS,
        SQL_API_SQLCOLATTRIBUTE,
        SQL_API_SQLSETDESCREC,
        SQL_API_SQLGETDESCREC,
        SQL_API_SQLSETDESCFIELD,
        SQL_API_SQLGETDESCFIELD,
        SQL_API_SQLCOPYDESC,

        // Data retrieval and LOB streaming
        SQL_API_SQLROWCOUNT,
        SQL_API_SQLGETDATA,
        SQL_API_SQLPARAMDATA,
        SQL_API_SQLPUTDATA,
        SQL_API_SQLGETDIAGFIELD,
        SQL_API_SQLGETDIAGREC,
        SQL_API_SQLERROR,

        // Catalog and metadata helpers
        SQL_API_SQLTABLES,
        SQL_API_SQLCOLUMNS,
        SQL_API_SQLPRIMARYKEYS,
        SQL_API_SQLFOREIGNKEYS,
        SQL_API_SQLSTATISTICS,
        SQL_API_SQLSPECIALCOLUMNS,
        SQL_API_SQLPROCEDURES,
        SQL_API_SQLPROCEDURECOLUMNS,
        SQL_API_SQLTABLEPRIVILEGES,
        SQL_API_SQLCOLUMNPRIVILEGES,

        // Capability/introspection
        SQL_API_SQLGETFUNCTIONS,
        SQL_API_SQLGETINFO,
        SQL_API_SQLGETTYPEINFO
    };

    if (function_id == 0) {
        // Both SQL_API_ALL_FUNCTIONS and SQL_API_ODBC3_ALL_FUNCTIONS are supported
        // in the same ODBC 3.x bitmap form in this driver build.
        function_id = SQL_API_ODBC3_ALL_FUNCTIONS;
    }

    if (function_id == SQL_API_ODBC3_ALL_FUNCTIONS) {
        // SQL_API_ODBC3_ALL_FUNCTIONS - return bitmap.
        std::memset(supported, 0,
                    static_cast<size_t>(SQL_API_ODBC3_ALL_FUNCTIONS_SIZE) * sizeof(SQLUSMALLINT));
        for (auto func : supported_functions) {
            size_t word = func >> 4;
            size_t bit = func & 0x0F;
            if (word < SQL_API_ODBC3_ALL_FUNCTIONS_SIZE) {
                supported[word] |= static_cast<SQLUSMALLINT>(1u << bit);
            }
        }
    } else {
        // Check specific function
        *supported = 0;
        for (auto func : supported_functions) {
            if (func == function_id) {
                *supported = 1;
                break;
            }
        }
    }

    return SQL_SUCCESS;
}

SQLRETURN OdbcConnection::getTypeInfo(SQLSMALLINT data_type, OdbcStatement* stmt) {
    clearDiagnostics();

    if (!stmt) {
        setError("HY009", 0, "Invalid use of null pointer");
        return SQL_ERROR;
    }

    std::vector<ColumnMetadata> cols;
    cols.push_back(makeCatalogColumn("TYPE_NAME", SQL_VARCHAR, 64));
    cols.push_back(makeCatalogColumn("DATA_TYPE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("COLUMN_SIZE", SQL_INTEGER));
    cols.push_back(makeCatalogColumn("LITERAL_PREFIX", SQL_VARCHAR, 32));
    cols.push_back(makeCatalogColumn("LITERAL_SUFFIX", SQL_VARCHAR, 32));
    cols.push_back(makeCatalogColumn("CREATE_PARAMS", SQL_VARCHAR, 32));
    cols.push_back(makeCatalogColumn("NULLABLE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("CASE_SENSITIVE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("SEARCHABLE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("UNSIGNED_ATTRIBUTE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("FIXED_PREC_SCALE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("AUTO_UNIQUE_VALUE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("LOCAL_TYPE_NAME", SQL_VARCHAR, 64));
    cols.push_back(makeCatalogColumn("MINIMUM_SCALE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("MAXIMUM_SCALE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("SQL_DATA_TYPE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("SQL_DATETIME_SUB", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("NUM_PREC_RADIX", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("INTERVAL_PRECISION", SQL_SMALLINT));

    std::vector<std::vector<std::string>> rows;
    bool all_types = (data_type == SQL_UNKNOWN_TYPE);
    for (const auto& entry : kTypeInfoEntries) {
        if (!all_types && entry.data_type != data_type) {
            continue;
        }
        rows.push_back({
            entry.type_name,
            std::to_string(entry.data_type),
            std::to_string(entry.column_size),
            entry.literal_prefix ? entry.literal_prefix : "",
            entry.literal_suffix ? entry.literal_suffix : "",
            entry.create_params ? entry.create_params : "",
            std::to_string(entry.nullable),
            std::to_string(entry.case_sensitive),
            std::to_string(entry.searchable),
            std::to_string(entry.unsigned_attr),
            std::to_string(entry.fixed_prec_scale),
            std::to_string(entry.auto_unique),
            entry.local_type_name ? entry.local_type_name : "",
            std::to_string(entry.min_scale),
            std::to_string(entry.max_scale),
            std::to_string(entry.sql_data_type),
            std::to_string(entry.sql_datetime_sub),
            std::to_string(entry.num_prec_radix),
            std::to_string(entry.interval_precision)
        });
    }

    stmt->setCatalogResult(std::move(cols), std::move(rows));
    return SQL_SUCCESS;
}

OdbcStatement* OdbcConnection::createStatement() {
    std::lock_guard lock(statements_mutex_);
    auto stmt = std::make_unique<OdbcStatement>(this);
    auto* ptr = stmt.get();
    statements_.push_back(std::move(stmt));
    return ptr;
}

void OdbcConnection::removeStatement(OdbcStatement* stmt) {
    std::lock_guard lock(statements_mutex_);
    statements_.erase(
        std::remove_if(statements_.begin(), statements_.end(),
                       [stmt](const auto& s) { return s.get() == stmt; }),
        statements_.end());
}

size_t OdbcConnection::getStatementCount() const {
    std::lock_guard lock(statements_mutex_);
    return statements_.size();
}

SQLRETURN OdbcConnection::parseConnectionString(const std::string& conn_str) {
    std::map<std::string, std::string> params;
    scratchbird::client::parseKeyValueConnectionString(conn_str, params, nullptr);

    std::string dsn_value;
    for (const auto& entry : params) {
        if (toLower(entry.first) == "dsn") {
            dsn_value = entry.second;
            break;
        }
    }
    if (!dsn_value.empty()) {
        auto dsn_result = applyDsnConfig(dsn_value);
        if (dsn_result != SQL_SUCCESS) {
            return dsn_result;
        }
    }

    for (const auto& entry : params) {
        const std::string key = toLower(entry.first);
        const std::string& value = entry.second;

        if (key == "driver") {
            params_.driver = value;
        } else if (key == "dsn") {
            params_.dsn = value;
        } else if (key == "server" || key == "host") {
            params_.server = value;
        } else if (key == "port") {
            try {
                params_.port = static_cast<uint16_t>(std::stoul(value));
            } catch (...) {
            }
        } else if (key == "database" || key == "db") {
            params_.database = value;
        } else if (key == "uid" || key == "user" || key == "username") {
            params_.user = value;
        } else if (key == "pwd" || key == "password") {
            params_.password = value;
        } else if (key == "ssl" || key == "sslmode") {
            params_.ssl_mode = value;
        } else if (key == "sslcert") {
            params_.ssl_cert = value;
        } else if (key == "sslkey") {
            params_.ssl_key = value;
        } else if (key == "sslrootcert") {
            params_.ssl_root_cert = value;
        } else if (key == "protocol") {
            std::string normalized;
            if (!normalizeNativeProtocol(value, normalized)) {
                setError("08001", 0,
                         "Only protocol=native is supported; connect to a native parser listener/port.");
                return SQL_ERROR;
            }
            params_.protocol = normalized;
        } else if (key == "front_door_mode" || key == "frontdoormode" ||
                   key == "connection_mode" || key == "ingress_mode") {
            params_.front_door_mode = value;
        } else if (key == "manager_auth_token" || key == "mcp_auth_token") {
            params_.manager_auth_token = value;
        } else if (key == "manager_username" || key == "mcp_username") {
            params_.manager_username = value;
        } else if (key == "manager_database" || key == "mcp_database") {
            params_.manager_database = value;
        } else if (key == "manager_connection_profile" || key == "mcp_connection_profile") {
            params_.manager_connection_profile = value;
        } else if (key == "manager_client_intent" || key == "mcp_client_intent") {
            params_.manager_client_intent = value;
        } else if (key == "manager_client_flags" || key == "mcp_client_flags") {
            try {
                params_.manager_client_flags = static_cast<uint16_t>(std::stoul(value));
            } catch (...) {
            }
        } else if (key == "manager_auth_fast_path" || key == "mcp_auth_fast_path") {
            const auto lower = toLower(value);
            params_.manager_auth_fast_path = (lower == "true" || lower == "1" || lower == "yes");
        } else if (key == "client_flags" || key == "connect_client_flags") {
            try {
                params_.connect_client_flags = static_cast<uint16_t>(std::stoul(value));
            } catch (...) {
            }
        } else if (key == "auth_method_id" || key == "authmethodid") {
            params_.auth_method_id = value;
        } else if (key == "auth_token" || key == "authtoken" ||
                   key == "bearer_token" || key == "bearertoken" ||
                   key == "token") {
            params_.auth_token = value;
        } else if (key == "auth_method_payload" || key == "authmethodpayload") {
            params_.auth_method_payload = value;
        } else if (key == "auth_payload_json" || key == "authpayloadjson") {
            params_.auth_payload_json = value;
        } else if (key == "auth_payload_b64" || key == "authpayloadb64") {
            params_.auth_payload_b64 = value;
        } else if (key == "auth_provider_profile" || key == "authproviderprofile") {
            params_.auth_provider_profile = value;
        } else if (key == "auth_required_methods" || key == "authrequiredmethods") {
            params_.auth_required_methods = value;
        } else if (key == "auth_forbidden_methods" || key == "authforbiddenmethods") {
            params_.auth_forbidden_methods = value;
        } else if (key == "auth_require_channel_binding" || key == "authrequirechannelbinding") {
            const auto lower = toLower(value);
            params_.auth_require_channel_binding = (lower == "true" || lower == "1" || lower == "yes" || lower == "on");
        } else if (key == "workload_identity_token" || key == "workloadidentitytoken") {
            params_.workload_identity_token = value;
        } else if (key == "proxy_principal_assertion" || key == "proxyprincipalassertion" || key == "proxy_assertion") {
            params_.proxy_principal_assertion = value;
        } else if (key == "timeout" || key == "connecttimeout") {
            try {
                params_.connect_timeout = static_cast<uint32_t>(std::stoul(value));
            } catch (...) {
            }
        } else if (key == "querytimeout") {
            try {
                params_.query_timeout = static_cast<uint32_t>(std::stoul(value));
            } catch (...) {
            }
        } else if (key == "applicationname" || key == "application_name" || key == "app") {
            params_.application_name = value;
        } else if (key == "schema" || key == "currentschema") {
            params_.schema = value;
        } else if (key == "charset" || key == "encoding") {
            params_.charset = value;
        } else if (key == "readonly") {
            params_.read_only = (value == "true" || value == "1" || value == "yes");
        } else if (key == "autocommit") {
            params_.auto_commit = (value == "true" || value == "1" || value == "yes");
        } else if (key == "packetsize") {
            try {
                params_.packet_size = static_cast<uint32_t>(std::stoul(value));
            } catch (...) {
            }
        } else if (key == "pooling") {
            params_.pooling = (value == "true" || value == "1" || value == "yes");
        }
    }

    return SQL_SUCCESS;
}

SQLRETURN OdbcConnection::applyDsnConfig(const std::string& dsn_name) {
    if (dsn_name.empty()) {
        return SQL_SUCCESS;
    }

    std::map<std::string, std::string> entries;
    if (!loadIniSection(dsn_name, entries)) {
        setError("IM002", 0, "Data source name not found and no default driver specified");
        return SQL_ERROR;
    }

    params_.dsn = dsn_name;

    auto getEntry = [&](const char* key) -> std::string {
        auto it = entries.find(toLower(key));
        if (it == entries.end()) {
            return {};
        }
        return it->second;
    };

    auto driver = getEntry("driver");
    if (!driver.empty()) {
        params_.driver = driver;
    }

    auto server = getEntry("server");
    if (server.empty()) {
        server = getEntry("host");
    }
    if (!server.empty()) {
        params_.server = server;
    }

    auto port = getEntry("port");
    if (!port.empty()) {
        try {
            params_.port = static_cast<uint16_t>(std::stoul(port));
        } catch (...) {
        }
    }

    auto database = getEntry("database");
    if (database.empty()) {
        database = getEntry("db");
    }
    if (!database.empty()) {
        params_.database = database;
    }

    if (params_.user.empty()) {
        auto uid = getEntry("uid");
        if (uid.empty()) {
            uid = getEntry("user");
        }
        if (uid.empty()) {
            uid = getEntry("username");
        }
        if (!uid.empty()) {
            params_.user = uid;
        }
    }

    if (params_.password.empty()) {
        auto pwd = getEntry("pwd");
        if (pwd.empty()) {
            pwd = getEntry("password");
        }
        if (!pwd.empty()) {
            params_.password = pwd;
        }
    }

    auto ssl_mode = getEntry("ssl");
    if (ssl_mode.empty()) {
        ssl_mode = getEntry("sslmode");
    }
    if (!ssl_mode.empty()) {
        params_.ssl_mode = ssl_mode;
    }

    auto ssl_cert = getEntry("sslcert");
    if (!ssl_cert.empty()) {
        params_.ssl_cert = ssl_cert;
    }

    auto ssl_key = getEntry("sslkey");
    if (!ssl_key.empty()) {
        params_.ssl_key = ssl_key;
    }

    auto ssl_root = getEntry("sslrootcert");
    if (!ssl_root.empty()) {
        params_.ssl_root_cert = ssl_root;
    }

    auto protocol = getEntry("protocol");
    if (!protocol.empty()) {
        std::string normalized;
        if (!normalizeNativeProtocol(protocol, normalized)) {
            setError("08001", 0,
                     "Only protocol=native is supported; connect to a native parser listener/port.");
            return SQL_ERROR;
        }
        params_.protocol = normalized;
    }

    auto front_door_mode = getEntry("front_door_mode");
    if (front_door_mode.empty()) {
        front_door_mode = getEntry("frontdoormode");
    }
    if (front_door_mode.empty()) {
        front_door_mode = getEntry("connection_mode");
    }
    if (front_door_mode.empty()) {
        front_door_mode = getEntry("ingress_mode");
    }
    if (!front_door_mode.empty()) {
        params_.front_door_mode = front_door_mode;
    }

    auto manager_auth_token = getEntry("manager_auth_token");
    if (manager_auth_token.empty()) {
        manager_auth_token = getEntry("mcp_auth_token");
    }
    if (!manager_auth_token.empty()) {
        params_.manager_auth_token = manager_auth_token;
    }

    auto manager_username = getEntry("manager_username");
    if (manager_username.empty()) {
        manager_username = getEntry("mcp_username");
    }
    if (!manager_username.empty()) {
        params_.manager_username = manager_username;
    }

    auto manager_database = getEntry("manager_database");
    if (manager_database.empty()) {
        manager_database = getEntry("mcp_database");
    }
    if (!manager_database.empty()) {
        params_.manager_database = manager_database;
    }

    auto manager_profile = getEntry("manager_connection_profile");
    if (manager_profile.empty()) {
        manager_profile = getEntry("mcp_connection_profile");
    }
    if (!manager_profile.empty()) {
        params_.manager_connection_profile = manager_profile;
    }

    auto manager_intent = getEntry("manager_client_intent");
    if (manager_intent.empty()) {
        manager_intent = getEntry("mcp_client_intent");
    }
    if (!manager_intent.empty()) {
        params_.manager_client_intent = manager_intent;
    }

    auto manager_flags = getEntry("manager_client_flags");
    if (manager_flags.empty()) {
        manager_flags = getEntry("mcp_client_flags");
    }
    if (!manager_flags.empty()) {
        try {
            params_.manager_client_flags = static_cast<uint16_t>(std::stoul(manager_flags));
        } catch (...) {
        }
    }

    auto manager_fast_path = toLower(getEntry("manager_auth_fast_path"));
    if (manager_fast_path.empty()) {
        manager_fast_path = toLower(getEntry("mcp_auth_fast_path"));
    }
    if (!manager_fast_path.empty()) {
        params_.manager_auth_fast_path =
            (manager_fast_path == "true" || manager_fast_path == "1" || manager_fast_path == "yes");
    }

    auto client_flags = getEntry("client_flags");
    if (client_flags.empty()) {
        client_flags = getEntry("connect_client_flags");
    }
    if (!client_flags.empty()) {
        try {
            params_.connect_client_flags = static_cast<uint16_t>(std::stoul(client_flags));
        } catch (...) {
        }
    }

    auto auth_method_id = getEntry("auth_method_id");
    if (auth_method_id.empty()) {
        auth_method_id = getEntry("authmethodid");
    }
    if (!auth_method_id.empty()) {
        params_.auth_method_id = auth_method_id;
    }

    auto auth_token = getEntry("auth_token");
    if (auth_token.empty()) {
        auth_token = getEntry("authtoken");
    }
    if (auth_token.empty()) {
        auth_token = getEntry("bearer_token");
    }
    if (auth_token.empty()) {
        auth_token = getEntry("bearertoken");
    }
    if (auth_token.empty()) {
        auth_token = getEntry("token");
    }
    if (!auth_token.empty()) {
        params_.auth_token = auth_token;
    }

    auto auth_method_payload = getEntry("auth_method_payload");
    if (auth_method_payload.empty()) {
        auth_method_payload = getEntry("authmethodpayload");
    }
    if (!auth_method_payload.empty()) {
        params_.auth_method_payload = auth_method_payload;
    }

    auto auth_payload_json = getEntry("auth_payload_json");
    if (auth_payload_json.empty()) {
        auth_payload_json = getEntry("authpayloadjson");
    }
    if (!auth_payload_json.empty()) {
        params_.auth_payload_json = auth_payload_json;
    }

    auto auth_payload_b64 = getEntry("auth_payload_b64");
    if (auth_payload_b64.empty()) {
        auth_payload_b64 = getEntry("authpayloadb64");
    }
    if (!auth_payload_b64.empty()) {
        params_.auth_payload_b64 = auth_payload_b64;
    }

    auto auth_provider_profile = getEntry("auth_provider_profile");
    if (auth_provider_profile.empty()) {
        auth_provider_profile = getEntry("authproviderprofile");
    }
    if (!auth_provider_profile.empty()) {
        params_.auth_provider_profile = auth_provider_profile;
    }

    auto auth_required_methods = getEntry("auth_required_methods");
    if (auth_required_methods.empty()) {
        auth_required_methods = getEntry("authrequiredmethods");
    }
    if (!auth_required_methods.empty()) {
        params_.auth_required_methods = auth_required_methods;
    }

    auto auth_forbidden_methods = getEntry("auth_forbidden_methods");
    if (auth_forbidden_methods.empty()) {
        auth_forbidden_methods = getEntry("authforbiddenmethods");
    }
    if (!auth_forbidden_methods.empty()) {
        params_.auth_forbidden_methods = auth_forbidden_methods;
    }

    auto auth_require_channel_binding = toLower(getEntry("auth_require_channel_binding"));
    if (auth_require_channel_binding.empty()) {
        auth_require_channel_binding = toLower(getEntry("authrequirechannelbinding"));
    }
    if (!auth_require_channel_binding.empty()) {
        params_.auth_require_channel_binding =
            (auth_require_channel_binding == "true" ||
             auth_require_channel_binding == "1" ||
             auth_require_channel_binding == "yes" ||
             auth_require_channel_binding == "on");
    }

    auto workload_identity_token = getEntry("workload_identity_token");
    if (workload_identity_token.empty()) {
        workload_identity_token = getEntry("workloadidentitytoken");
    }
    if (!workload_identity_token.empty()) {
        params_.workload_identity_token = workload_identity_token;
    }

    auto proxy_principal_assertion = getEntry("proxy_principal_assertion");
    if (proxy_principal_assertion.empty()) {
        proxy_principal_assertion = getEntry("proxyprincipalassertion");
    }
    if (proxy_principal_assertion.empty()) {
        proxy_principal_assertion = getEntry("proxy_assertion");
    }
    if (!proxy_principal_assertion.empty()) {
        params_.proxy_principal_assertion = proxy_principal_assertion;
    }

    auto timeout = getEntry("timeout");
    if (timeout.empty()) {
        timeout = getEntry("connecttimeout");
    }
    if (!timeout.empty()) {
        try {
            params_.connect_timeout = static_cast<uint32_t>(std::stoul(timeout));
        } catch (...) {
        }
    }

    auto query_timeout = getEntry("querytimeout");
    if (!query_timeout.empty()) {
        try {
            params_.query_timeout = static_cast<uint32_t>(std::stoul(query_timeout));
        } catch (...) {
        }
    }

    auto app_name = getEntry("applicationname");
    if (app_name.empty()) {
        app_name = getEntry("application_name");
    }
    if (app_name.empty()) {
        app_name = getEntry("app");
    }
    if (!app_name.empty()) {
        params_.application_name = app_name;
    }

    auto schema = getEntry("schema");
    if (schema.empty()) {
        schema = getEntry("currentschema");
    }
    if (!schema.empty()) {
        params_.schema = schema;
    }

    auto charset = getEntry("charset");
    if (charset.empty()) {
        charset = getEntry("encoding");
    }
    if (!charset.empty()) {
        params_.charset = charset;
    }

    auto read_only = toLower(getEntry("readonly"));
    if (!read_only.empty()) {
        params_.read_only = (read_only == "true" || read_only == "1" || read_only == "yes");
    }

    auto auto_commit = toLower(getEntry("autocommit"));
    if (!auto_commit.empty()) {
        params_.auto_commit = (auto_commit == "true" || auto_commit == "1" || auto_commit == "yes");
    }

    auto packet_size = getEntry("packetsize");
    if (!packet_size.empty()) {
        try {
            params_.packet_size = static_cast<uint32_t>(std::stoul(packet_size));
        } catch (...) {
        }
    }

    auto pooling = toLower(getEntry("pooling"));
    if (!pooling.empty()) {
        params_.pooling = (pooling == "true" || pooling == "1" || pooling == "yes");
    }

    return SQL_SUCCESS;
}

SQLRETURN OdbcConnection::establishConnection() {
    constexpr const char* kDefaultSessionSchema = "users.public";
    if (!client_bridge_) {
        setError("08001", 0, "Client bridge not initialized");
        return SQL_ERROR;
    }

    std::string normalized_protocol;
    if (!normalizeNativeProtocol(params_.protocol, normalized_protocol)) {
        setError("08001", 0,
                 "Only protocol=native is supported; connect to a native parser listener/port.");
        return SQL_ERROR;
    }
    params_.protocol = normalized_protocol;

    std::string error;
    auto connect_result = client_bridge_->connect(params_, error);
    if (connect_result != SQL_SUCCESS) {
        setError("08001", 0, error.empty() ? "Failed to connect" : error);
        return connect_result;
    }

    connected_ = true;
    current_database_ = params_.database;
    current_user_ = params_.user;
    current_schema_ = params_.schema.empty() ? kDefaultSessionSchema : params_.schema;

    if (params_.read_only) {
        access_mode_ = SQL_MODE_READ_ONLY;
    }
    auto_commit_ = params_.auto_commit ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF;
    login_timeout_ = params_.connect_timeout;
    in_transaction_ = (auto_commit_ == SQL_AUTOCOMMIT_OFF);

    auto result = applyIsolationSetting();
    if (result != SQL_SUCCESS && result != SQL_SUCCESS_WITH_INFO) {
        if (client_bridge_) {
            client_bridge_->disconnect();
        }
        connected_ = false;
        return result;
    }

    result = applyAutocommitSetting();
    if (result != SQL_SUCCESS && result != SQL_SUCCESS_WITH_INFO) {
        if (client_bridge_) {
            client_bridge_->disconnect();
        }
        connected_ = false;
        return result;
    }

    registerResilience();
    return SQL_SUCCESS;
}

bool OdbcConnection::allowRequest() {
    if (!circuit_breaker_.AllowRequest()) {
        setError("08006", 0, "Circuit breaker is OPEN");
        return false;
    }
    return true;
}

void OdbcConnection::recordSuccess() {
    circuit_breaker_.RecordSuccess();
    if (keepalive_tracker_) {
        keepalive_tracker_->MarkActive();
    }
}

void OdbcConnection::recordFailure() {
    circuit_breaker_.RecordFailure();
}

void OdbcConnection::registerResilience() {
    if (!env_) {
        return;
    }
    keepalive_tracker_ = env_->keepaliveManager().Register(connection_id_, static_cast<SQLHDBC>(this));
    leak_guard_ = env_->leakDetector().Checkout(connection_id_);
}

void OdbcConnection::unregisterResilience() {
    if (!env_) {
        return;
    }
    env_->keepaliveManager().Unregister(connection_id_);
    keepalive_tracker_ = nullptr;
    leak_guard_.reset();
}

std::string OdbcConnection::buildConnectionString() const {
    std::ostringstream ss;

    if (!params_.driver.empty()) {
        ss << "Driver={" << params_.driver << "};";
    }
    if (!params_.server.empty()) {
        ss << "Server=" << params_.server << ";";
    }
    ss << "Port=" << params_.port << ";";
    if (!params_.database.empty()) {
        ss << "Database=" << params_.database << ";";
    }
    if (!params_.user.empty()) {
        ss << "UID=" << params_.user << ";";
    }
    // Don't include password in output string for security

    return ss.str();
}

SQLRETURN OdbcConnection::applyAutocommitSetting() {
    if (!connected_) {
        setError("08003", 0, "Connection not open");
        return SQL_ERROR;
    }

    std::vector<std::vector<std::string>> results;
    std::vector<ColumnMetadata> columns;
    SQLLEN rows_affected = 0;
    std::string sql = buildAutocommitSql(auto_commit_);
    return executeSQL(sql, results, columns, rows_affected);
}

SQLRETURN OdbcConnection::applyIsolationSetting() {
    if (!connected_) {
        setError("08003", 0, "Connection not open");
        return SQL_ERROR;
    }

    std::string sql;
    if (!buildIsolationSql(txn_isolation_, sql)) {
        setError("HY024", 0, "Invalid attribute value");
        return SQL_ERROR;
    }

    std::vector<std::vector<std::string>> results;
    std::vector<ColumnMetadata> columns;
    SQLLEN rows_affected = 0;
    return executeSQL(sql, results, columns, rows_affected);
}

SQLRETURN OdbcConnection::executeSQL(const std::string& sql,
                                      std::vector<std::vector<std::string>>& results,
                                      std::vector<ColumnMetadata>& columns,
                                      SQLLEN& rows_affected) {
    if (!client_bridge_) {
        setError("08003", 0, "Connection not open");
        return SQL_ERROR;
    }
    if (!allowRequest()) {
        return SQL_ERROR;
    }
    auto result = client_bridge_->executeSQL(sql, results, columns, rows_affected);
    if (result != SQL_SUCCESS && result != SQL_SUCCESS_WITH_INFO) {
        auto status = client_bridge_->lastStatus();
        auto message = client_bridge_->lastError();
        setError(mapStatusToSqlState(status), static_cast<SQLINTEGER>(status),
                 message.empty() ? "Execution failed" : message);
        recordFailure();
    } else {
        recordSuccess();
    }
    return result;
}

SQLRETURN OdbcConnection::cancel() {
    if (!client_bridge_) {
        setError("08003", 0, "Connection not open");
        return SQL_ERROR;
    }
    auto result = client_bridge_->cancel();
    if (result != SQL_SUCCESS) {
        auto status = client_bridge_->lastStatus();
        auto message = client_bridge_->lastError();
        setError(mapStatusToSqlState(status), static_cast<SQLINTEGER>(status),
                 message.empty() ? "Cancel failed" : message);
    }
    return result;
}

SQLRETURN OdbcConnection::prepareSQL(const std::string& sql, uint64_t& stmt_id,
                                      std::vector<ColumnMetadata>& /*param_metadata*/) {
    static std::atomic<uint64_t> next_stmt_id{1};
    stmt_id = next_stmt_id++;
    prepared_sql_[stmt_id] = sql;
    return SQL_SUCCESS;
}

SQLRETURN OdbcConnection::executePrepared(uint64_t stmt_id,
                                           const std::vector<ParameterLiteral>& params,
                                           std::vector<std::vector<std::string>>& results,
                                           std::vector<ColumnMetadata>& columns,
                                           SQLLEN& rows_affected) {
    std::string sql;
    auto status = buildPreparedSQL(stmt_id, params, sql);
    if (status != SQL_SUCCESS) {
        return status;
    }

    return executeSQL(sql, results, columns, rows_affected);
}

SQLRETURN OdbcConnection::buildPreparedSQL(uint64_t stmt_id,
                                            const std::vector<ParameterLiteral>& params,
                                            std::string& out_sql) {
    auto it = prepared_sql_.find(stmt_id);
    if (it == prepared_sql_.end()) {
        setError("HY000", 0, "Unknown prepared statement");
        return SQL_ERROR;
    }

    std::string sql = it->second;
    if (!params.empty()) {
        std::string out;
        out.reserve(sql.size() + params.size() * 8);
        size_t param_index = 0;
        for (char ch : sql) {
            if (ch == '?' && param_index < params.size()) {
                const auto& param = params[param_index++];
                if (param.text.empty()) {
                    out += "NULL";
                } else if (param.quoted) {
                    out += "'";
                    for (char c : param.text) {
                        if (c == '\'') {
                            out += "''";
                        } else {
                            out.push_back(c);
                        }
                    }
                    out += "'";
                } else {
                    out += param.text;
                }
            } else {
                out.push_back(ch);
            }
        }
        sql = std::move(out);
    }

    out_sql = compactPreparedSql(std::move(sql));
    return SQL_SUCCESS;
}

// =============================================================================
// OdbcStatement Implementation
// =============================================================================

OdbcStatement::OdbcStatement(OdbcConnection* conn)
    : conn_(conn) {
    owned_app_param_desc_ = std::make_unique<OdbcDescriptor>(conn, OdbcDescriptor::DescriptorType::APD, true);
    owned_imp_param_desc_ = std::make_unique<OdbcDescriptor>(conn, OdbcDescriptor::DescriptorType::IPD, true);
    owned_app_row_desc_ = std::make_unique<OdbcDescriptor>(conn, OdbcDescriptor::DescriptorType::ARD, true);
    owned_ird_desc_ = std::make_unique<OdbcDescriptor>(conn, OdbcDescriptor::DescriptorType::IRD, true);

    app_param_desc_ = owned_app_param_desc_.get();
    ipd_desc_ = owned_imp_param_desc_.get();
    app_row_desc_ = owned_app_row_desc_.get();
    ird_desc_ = owned_ird_desc_.get();
    cursor_name_ = "SB_CUR_" + std::to_string(kConnectionIdCounter.fetch_add(1) + 1);
}

OdbcStatement::~OdbcStatement() = default;

SQLRETURN OdbcStatement::prepare(const SQLCHAR* sql, SQLINTEGER sql_len) {
    clearDiagnostics();

    if (!sql) {
        setError("HY009", 0, "Invalid use of null pointer");
        return SQL_ERROR;
    }

    sql_ = (sql_len == SQL_NTS) ?
        std::string(reinterpret_cast<const char*>(sql)) :
        std::string(reinterpret_cast<const char*>(sql), sql_len);

    std::vector<ColumnMetadata> param_metadata;
    auto result = conn_->prepareSQL(sql_, server_stmt_id_, param_metadata);
    if (result == SQL_SUCCESS) {
        prepared_ = true;
        executed_ = false;
        clearPutDataState();
    }

    return result;
}

SQLRETURN OdbcStatement::execute() {
    clearDiagnostics();

    if (!prepared_) {
        setError("HY010", 0, "Function sequence error");
        return SQL_ERROR;
    }

    if (data_at_exec_active_) {
        return SQL_NEED_DATA;
    }

    auto init_data_at_exec = validateOrInitDataAtExecState();
    if (init_data_at_exec == SQL_NEED_DATA) {
        return SQL_NEED_DATA;
    }
    if (init_data_at_exec != SQL_SUCCESS) {
        return init_data_at_exec;
    }

    std::vector<ParameterLiteral> params;
    auto build_status = buildParameterData(params, 0);
    if (build_status == SQL_NEED_DATA) {
        return SQL_NEED_DATA;
    }
    if (build_status != SQL_SUCCESS && build_status != SQL_SUCCESS_WITH_INFO) {
        return build_status;
    }

    std::string sql;
    auto status = conn_->buildPreparedSQL(server_stmt_id_, params, sql);
    if (status != SQL_SUCCESS) {
        clearPutDataState();
        return status;
    }

    auto execute_status = executeSqlStatements(sql);
    clearPutDataState();
    return execute_status;
}

SQLRETURN OdbcStatement::execDirect(const SQLCHAR* sql, SQLINTEGER sql_len) {
    clearDiagnostics();

    if (!sql) {
        setError("HY009", 0, "Invalid use of null pointer");
        return SQL_ERROR;
    }

    sql_ = (sql_len == SQL_NTS) ?
        std::string(reinterpret_cast<const char*>(sql)) :
        std::string(reinterpret_cast<const char*>(sql), sql_len);

    auto result = executeSqlStatements(sql_);
    clearPutDataState();
    if (result == SQL_SUCCESS || result == SQL_SUCCESS_WITH_INFO) {
        prepared_ = false;
    }

    return result;
}

SQLRETURN OdbcStatement::cancel() {
    clearDiagnostics();
    if (!conn_) {
        setError("08003", 0, "Connection not open");
        return SQL_ERROR;
    }
    return conn_->cancel();
}

SQLRETURN OdbcStatement::closeCursor() {
    clearDiagnostics();

    if (!has_results_) {
        setError("24000", 0, "Invalid cursor state");
        return SQL_ERROR;
    }

    resetResults();

    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::freeStmt(SQLUSMALLINT option) {
    clearDiagnostics();

    switch (option) {
        case SQL_CLOSE:
            if (has_results_) {
                closeCursor();
            }
            break;

        case SQL_DROP:
            // Will be handled by destructor
            break;

        case SQL_UNBIND:
            col_bindings_.clear();
            if (app_row_desc_) {
                app_row_desc_->resetDescriptor();
            }
            if (ird_desc_) {
                ird_desc_->resetDescriptor();
            }
            break;

        case SQL_RESET_PARAMS:
            param_bindings_.clear();
            if (app_param_desc_) {
                app_param_desc_->resetDescriptor();
            }
            if (ipd_desc_) {
                ipd_desc_->resetDescriptor();
            }
            clearPutDataState();
            break;

        default:
            setError("HY092", 0, "Invalid attribute identifier");
            return SQL_ERROR;
    }

    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::bindParameter(SQLUSMALLINT parameter_number,
                                        SQLSMALLINT input_output_type,
                                        SQLSMALLINT value_type,
                                        SQLSMALLINT parameter_type,
                                        SQLULEN column_size,
                                        SQLSMALLINT decimal_digits,
                                        SQLPOINTER parameter_value,
                                        SQLLEN buffer_length,
                                        SQLLEN* str_len_or_ind) {
    clearDiagnostics();
    clearPutDataState();

    if (parameter_number == 0) {
        setError("HY000", 0, "Invalid parameter number");
        return SQL_ERROR;
    }

    ParameterBinding binding;
    binding.input_output_type = input_output_type;
    binding.value_type = value_type;
    binding.parameter_type = parameter_type;
    binding.column_size = column_size;
    binding.decimal_digits = decimal_digits;
    binding.parameter_value = parameter_value;
    binding.buffer_length = buffer_length;
    binding.str_len_or_ind = str_len_or_ind;

    param_bindings_[parameter_number] = binding;

    if (!app_param_desc_) {
        setError("HY024", 0, "Statement not initialized");
        return SQL_ERROR;
    }

    SQLSMALLINT count = static_cast<SQLSMALLINT>(parameter_number);

    auto update_param_descriptor = [&](OdbcDescriptor* desc) -> SQLRETURN {
        if (!desc) {
            return SQL_SUCCESS;
        }

        auto unsigned_flag = SQLSMALLINT(0);
        switch (value_type) {
            case SQL_C_UTINYINT:
            case SQL_C_USHORT:
            case SQL_C_ULONG:
            case SQL_C_UBIGINT:
                unsigned_flag = 1;
                break;
            default:
                break;
        }

        SQLSMALLINT nullability = SQL_NULLABLE_UNKNOWN;
        SQLSMALLINT precision = static_cast<SQLSMALLINT>(column_size <= 32767 ? column_size : 32767);
        SQLSMALLINT scale = decimal_digits;
        SQLSMALLINT searchable = SQL_SEARCHABLE;
        if (desc->setField(0, SQL_DESC_COUNT, &count, 0) != SQL_SUCCESS) {
            return SQL_ERROR;
        }
        if (desc->setField(parameter_number, SQL_DESC_TYPE, &parameter_type, 0) != SQL_SUCCESS) {
            return SQL_ERROR;
        }
        if (desc->setField(parameter_number, SQL_DESC_CONCISE_TYPE, &parameter_type, 0) != SQL_SUCCESS) {
            return SQL_ERROR;
        }
        if (desc->setField(parameter_number, SQL_DESC_DATA_PTR, parameter_value, 0) != SQL_SUCCESS) {
            return SQL_ERROR;
        }
        if (desc->setField(parameter_number, SQL_DESC_INDICATOR_PTR, str_len_or_ind, 0) != SQL_SUCCESS) {
            return SQL_ERROR;
        }
        if (desc->setField(parameter_number, SQL_DESC_OCTET_LENGTH_PTR, str_len_or_ind, 0) != SQL_SUCCESS) {
            return SQL_ERROR;
        }
        if (desc->setField(parameter_number, SQL_DESC_LENGTH, &buffer_length, 0) != SQL_SUCCESS) {
            return SQL_ERROR;
        }
        if (desc->setField(parameter_number, SQL_DESC_OCTET_LENGTH, &buffer_length, 0) != SQL_SUCCESS) {
            return SQL_ERROR;
        }
        if (desc->setField(parameter_number, SQL_DESC_PRECISION, &precision, 0) != SQL_SUCCESS) {
            return SQL_ERROR;
        }
        if (desc->setField(parameter_number, SQL_DESC_SCALE, &scale, 0) != SQL_SUCCESS) {
            return SQL_ERROR;
        }
        if (desc->setField(parameter_number, SQL_DESC_UNSIGNED, &unsigned_flag, 0) != SQL_SUCCESS) {
            return SQL_ERROR;
        }
        if (desc->setField(parameter_number, SQL_DESC_NULLABLE, &nullability, 0) != SQL_SUCCESS) {
            return SQL_ERROR;
        }
        if (desc->setField(parameter_number, SQL_DESC_SEARCHABLE, &searchable, 0) != SQL_SUCCESS) {
            return SQL_ERROR;
        }

        return SQL_SUCCESS;
    };

    if (update_param_descriptor(app_param_desc_) != SQL_SUCCESS ||
        update_param_descriptor(ipd_desc_) != SQL_SUCCESS) {
        setError("HY000", 0, "Failed to update descriptor metadata");
        return SQL_ERROR;
    }

    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::numParams(SQLSMALLINT* param_count) {
    clearDiagnostics();

    if (!param_count) {
        setError("HY009", 0, "Invalid use of null pointer");
        return SQL_ERROR;
    }

    *param_count = static_cast<SQLSMALLINT>(param_bindings_.size());
    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::describeParam(SQLUSMALLINT parameter_number,
                                        SQLSMALLINT* data_type,
                                        SQLULEN* parameter_size,
                                        SQLSMALLINT* decimal_digits,
                                        SQLSMALLINT* nullable) {
    clearDiagnostics();

    auto it = param_bindings_.find(parameter_number);
    if (it == param_bindings_.end()) {
        setError("07009", 0, "Invalid descriptor index");
        return SQL_ERROR;
    }

    if (data_type) *data_type = it->second.parameter_type;
    if (parameter_size) *parameter_size = it->second.column_size;
    if (decimal_digits) *decimal_digits = it->second.decimal_digits;
    if (nullable) *nullable = SQL_NULLABLE_UNKNOWN;

    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::bindCol(SQLUSMALLINT column_number,
                                  SQLSMALLINT target_type,
                                  SQLPOINTER target_value,
                                  SQLLEN buffer_length,
                                  SQLLEN* str_len_or_ind) {
    clearDiagnostics();

    if (column_number == 0) {
        if (!target_value && !str_len_or_ind) {
            bookmark_bound_ = false;
            bookmark_binding_ = ColumnBinding{};
            return SQL_SUCCESS;
        }
        bookmark_binding_.target_type = target_type;
        bookmark_binding_.target_value = target_value;
        bookmark_binding_.buffer_length = buffer_length;
        bookmark_binding_.str_len_or_ind = str_len_or_ind;
        bookmark_bound_ = true;
        return SQL_SUCCESS;
    }

    ColumnBinding binding;
    binding.target_type = target_type;
    binding.target_value = target_value;
    binding.buffer_length = buffer_length;
    binding.str_len_or_ind = str_len_or_ind;

    col_bindings_[column_number] = binding;

    if (!app_row_desc_) {
        setError("HY024", 0, "Statement not initialized");
        return SQL_ERROR;
    }

    if (column_number > 0) {
        SQLSMALLINT desc_count = static_cast<SQLSMALLINT>(column_number);
        auto* col = column_number <= columns_.size() ? &columns_[column_number - 1] : nullptr;

        auto update_row_descriptor = [&](OdbcDescriptor* desc) -> SQLRETURN {
            if (!desc) {
                return SQL_SUCCESS;
            }

            if (desc->setField(0, SQL_DESC_COUNT, &desc_count, 0) != SQL_SUCCESS) {
                return SQL_ERROR;
            }
            if (desc->setField(column_number, SQL_DESC_DATA_PTR, target_value, 0) != SQL_SUCCESS) {
                return SQL_ERROR;
            }
            if (desc->setField(column_number, SQL_DESC_OCTET_LENGTH_PTR, nullptr, 0) != SQL_SUCCESS) {
                return SQL_ERROR;
            }
            if (desc->setField(column_number, SQL_DESC_INDICATOR_PTR, str_len_or_ind, 0) != SQL_SUCCESS) {
                return SQL_ERROR;
            }
            if (desc->setField(column_number, SQL_DESC_OCTET_LENGTH, &buffer_length, 0) != SQL_SUCCESS) {
                return SQL_ERROR;
            }
            if (col) {
                if (desc->setField(column_number, SQL_DESC_TYPE, &col->sql_type, 0) != SQL_SUCCESS) {
                    return SQL_ERROR;
                }
                SQLSMALLINT precision =
                    static_cast<SQLSMALLINT>(col->column_size <= 32767 ? col->column_size : 32767);
                SQLLEN length = static_cast<SQLLEN>(col->column_size);
                SQLSMALLINT scale = col->decimal_digits;
                SQLSMALLINT updatable = 0;  // SQL_ATTR_READONLY
                if (desc->setField(column_number, SQL_DESC_PRECISION, &precision, 0) != SQL_SUCCESS) {
                    return SQL_ERROR;
                }
                if (desc->setField(column_number, SQL_DESC_SCALE, &scale, 0) != SQL_SUCCESS) {
                    return SQL_ERROR;
                }
                if (desc->setField(column_number, SQL_DESC_NULLABLE, &col->nullable, 0) != SQL_SUCCESS) {
                    return SQL_ERROR;
                }
                if (desc->setField(column_number, SQL_DESC_LENGTH, &length, 0) != SQL_SUCCESS) {
                    return SQL_ERROR;
                }
                if (desc->setField(column_number, SQL_DESC_DISPLAY_SIZE, &length, 0) != SQL_SUCCESS) {
                    return SQL_ERROR;
                }
                if (desc->setField(column_number, SQL_DESC_SEARCHABLE, &col->searchable, 0) != SQL_SUCCESS) {
                    return SQL_ERROR;
                }
                if (desc->setField(column_number, SQL_DESC_UPDATABLE, &updatable, 0) != SQL_SUCCESS) {
                    return SQL_ERROR;
                }
            }
            return SQL_SUCCESS;
        };

        if (update_row_descriptor(app_row_desc_) != SQL_SUCCESS ||
            update_row_descriptor(ird_desc_) != SQL_SUCCESS) {
            setError("HY000", 0, "Failed to update row descriptor metadata");
            return SQL_ERROR;
        }
    }

    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::numResultCols(SQLSMALLINT* column_count) {
    clearDiagnostics();

    if (!column_count) {
        setError("HY009", 0, "Invalid use of null pointer");
        return SQL_ERROR;
    }

    *column_count = static_cast<SQLSMALLINT>(columns_.size());
    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::describeCol(SQLUSMALLINT column_number,
                                      SQLCHAR* column_name,
                                      SQLSMALLINT buffer_length,
                                      SQLSMALLINT* name_length,
                                      SQLSMALLINT* data_type,
                                      SQLULEN* column_size,
                                      SQLSMALLINT* decimal_digits,
                                      SQLSMALLINT* nullable) {
    clearDiagnostics();

    if (column_number == 0 || column_number > columns_.size()) {
        setError("07009", 0, "Invalid descriptor index");
        return SQL_ERROR;
    }

    const auto& col = columns_[column_number - 1];
    SQLRETURN result = SQL_SUCCESS;

    // Copy name
    if (name_length) {
        *name_length = static_cast<SQLSMALLINT>(col.name.size());
    }
    if (column_name && buffer_length > 0) {
        size_t copy_len = std::min(static_cast<size_t>(buffer_length - 1), col.name.size());
        std::memcpy(column_name, col.name.c_str(), copy_len);
        column_name[copy_len] = '\0';
        if (col.name.size() >= static_cast<size_t>(buffer_length)) {
            setError("01004", 0, "String data, right truncated");
            result = SQL_SUCCESS_WITH_INFO;
        }
    }

    if (data_type) *data_type = col.sql_type;
    if (column_size) *column_size = col.column_size;
    if (decimal_digits) *decimal_digits = col.decimal_digits;
    if (nullable) *nullable = col.nullable;

    return result;
}

SQLRETURN OdbcStatement::colAttribute(SQLUSMALLINT column_number,
                                       SQLUSMALLINT field_identifier,
                                       SQLPOINTER char_attr,
                                       SQLSMALLINT buffer_length,
                                       SQLSMALLINT* string_length,
                                       SQLLEN* numeric_attr) {
    clearDiagnostics();

    if (column_number == 0 || column_number > columns_.size()) {
        if (field_identifier == SQL_DESC_COUNT) {
            if (numeric_attr) *numeric_attr = static_cast<SQLLEN>(columns_.size());
            return SQL_SUCCESS;
        }
        setError("07009", 0, "Invalid descriptor index");
        return SQL_ERROR;
    }

    const auto& col = columns_[column_number - 1];

    auto copyString = [&](const std::string& str) -> SQLRETURN {
        if (string_length) {
            *string_length = static_cast<SQLSMALLINT>(str.size());
        }
        if (char_attr && buffer_length > 0) {
            size_t copy_len = std::min(static_cast<size_t>(buffer_length - 1), str.size());
            std::memcpy(char_attr, str.c_str(), copy_len);
            static_cast<char*>(char_attr)[copy_len] = '\0';
            if (str.size() >= static_cast<size_t>(buffer_length)) {
                setError("01004", 0, "String data, right truncated");
                return SQL_SUCCESS_WITH_INFO;
            }
        }
        return SQL_SUCCESS;
    };

    switch (field_identifier) {
        // Note: SQL_DESC_NAME = SQL_COLUMN_NAME (same value), only one case needed
        case SQL_DESC_NAME:  // Also SQL_COLUMN_NAME
            return copyString(col.name);

        case SQL_DESC_LABEL:  // Also SQL_COLUMN_LABEL
            return copyString(col.label.empty() ? col.name : col.label);

        case SQL_DESC_TYPE_NAME:  // Also SQL_COLUMN_TYPE_NAME
            return copyString(col.type_name);

        case SQL_DESC_TABLE_NAME:  // Also SQL_COLUMN_TABLE_NAME
            return copyString(col.table_name);

        case SQL_DESC_SCHEMA_NAME:  // Also SQL_COLUMN_OWNER_NAME
            return copyString(col.schema_name);

        case SQL_DESC_CATALOG_NAME:  // Also SQL_COLUMN_QUALIFIER_NAME
            return copyString(col.catalog_name);

        case SQL_DESC_TYPE:  // Also SQL_DESC_CONCISE_TYPE, SQL_COLUMN_TYPE
            if (numeric_attr) *numeric_attr = col.sql_type;
            break;

        case SQL_DESC_LENGTH:  // Also SQL_COLUMN_LENGTH
            if (numeric_attr) *numeric_attr = static_cast<SQLLEN>(col.column_size);
            break;

        case SQL_DESC_PRECISION:  // Also SQL_COLUMN_PRECISION
            if (numeric_attr) *numeric_attr = static_cast<SQLLEN>(col.column_size);
            break;

        case SQL_DESC_SCALE:  // Also SQL_COLUMN_SCALE
            if (numeric_attr) *numeric_attr = col.decimal_digits;
            break;

        case SQL_DESC_NULLABLE:  // Also SQL_COLUMN_NULLABLE
            if (numeric_attr) *numeric_attr = col.nullable;
            break;

        case SQL_DESC_UNSIGNED:  // Also SQL_COLUMN_UNSIGNED
            if (numeric_attr) *numeric_attr = col.unsigned_flag ? 1 : 0;
            break;

        case SQL_DESC_AUTO_UNIQUE_VALUE:  // Also SQL_COLUMN_AUTO_INCREMENT
            if (numeric_attr) *numeric_attr = col.auto_increment ? 1 : 0;
            break;

        case SQL_DESC_CASE_SENSITIVE:  // Also SQL_COLUMN_CASE_SENSITIVE
            if (numeric_attr) *numeric_attr = col.case_sensitive ? 1 : 0;
            break;

        case SQL_DESC_SEARCHABLE:  // Also SQL_COLUMN_SEARCHABLE
            if (numeric_attr) *numeric_attr = col.searchable;
            break;

        case SQL_DESC_DISPLAY_SIZE:  // Also SQL_COLUMN_DISPLAY_SIZE
            if (numeric_attr) *numeric_attr = col.display_size;
            break;

        case SQL_DESC_OCTET_LENGTH:
            if (numeric_attr) *numeric_attr = col.octet_length;
            break;

        case SQL_DESC_COUNT:
            if (numeric_attr) *numeric_attr = static_cast<SQLLEN>(columns_.size());
            break;

        case SQL_COLUMN_UPDATABLE:  // Also SQL_DESC_UPDATABLE
            if (numeric_attr) *numeric_attr = 0;  // SQL_ATTR_READONLY
            break;

        case SQL_COLUMN_MONEY:
            if (numeric_attr) *numeric_attr = 0;
            break;

        default:
            setError("HY091", 0, "Invalid descriptor field identifier");
            return SQL_ERROR;
    }

    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::setCursorName(const SQLCHAR* cursor_name, SQLSMALLINT name_length) {
    clearDiagnostics();

    if (!cursor_name) {
        setError("HY009", 0, "Invalid use of null pointer");
        return SQL_ERROR;
    }

    std::string requested_name;
    if (name_length == SQL_NTS) {
        requested_name = reinterpret_cast<const char*>(cursor_name);
    } else if (name_length < 0) {
        setError("HY090", 0, "Invalid string or buffer length");
        return SQL_ERROR;
    } else {
        requested_name.assign(reinterpret_cast<const char*>(cursor_name),
                              static_cast<size_t>(name_length));
    }

    if (requested_name.empty()) {
        setError("34000", 0, "Invalid cursor name");
        return SQL_ERROR;
    }

    cursor_name_ = requested_name;
    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::getCursorName(SQLCHAR* cursor_name, SQLSMALLINT buffer_length,
                                       SQLSMALLINT* name_length) {
    clearDiagnostics();

    if (name_length) {
        *name_length = static_cast<SQLSMALLINT>(cursor_name_.size());
    }
    if (!cursor_name) {
        return SQL_SUCCESS;
    }
    if (buffer_length <= 0) {
        setError("HY090", 0, "Invalid string or buffer length");
        return SQL_ERROR;
    }

    size_t copy_len = std::min(static_cast<size_t>(buffer_length - 1), cursor_name_.size());
    std::memcpy(cursor_name, cursor_name_.data(), copy_len);
    cursor_name[copy_len] = '\0';
    if (cursor_name_.size() >= static_cast<size_t>(buffer_length)) {
        setError("01004", 0, "String data, right truncated");
        return SQL_SUCCESS_WITH_INFO;
    }
    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::fetch() {
    clearDiagnostics();

    if (!has_results_) {
        setError("24000", 0, "Invalid cursor state");
        return SQL_ERROR;
    }

    size_t next_index = current_row_;
    if (next_index >= rows_.size()) {
        return SQL_NO_DATA;
    }

    current_row_ = next_index + 1;
    clearGetDataState();
    auto result = bindResultData();

    return result;
}

SQLRETURN OdbcStatement::fetchScroll(SQLSMALLINT fetch_orientation, SQLLEN fetch_offset) {
    clearDiagnostics();

    if (!has_results_) {
        setError("24000", 0, "Invalid cursor state");
        return SQL_ERROR;
    }

    if (rows_.empty()) {
        return SQL_NO_DATA;
    }

    if (cursor_type_ == SQL_CURSOR_FORWARD_ONLY &&
        fetch_orientation != SQL_FETCH_NEXT) {
        setError("HY106", 0, "Forward-only cursor does not support scroll fetch");
        return SQL_ERROR;
    }

    int64_t current_index = current_row_ == 0 ? -1 : static_cast<int64_t>(current_row_ - 1);
    int64_t new_index = current_index;

    switch (fetch_orientation) {
        case SQL_FETCH_NEXT:
            new_index = current_index + 1;
            break;
        case SQL_FETCH_FIRST:
            new_index = 0;
            break;
        case SQL_FETCH_LAST:
            new_index = static_cast<int64_t>(rows_.size() - 1);
            break;
        case SQL_FETCH_PRIOR:
            new_index = current_index - 1;
            break;
        case SQL_FETCH_ABSOLUTE:
            if (fetch_offset > 0) {
                new_index = static_cast<int64_t>(fetch_offset - 1);
            } else if (fetch_offset < 0) {
                new_index = static_cast<int64_t>(rows_.size()) + fetch_offset;
            } else {
                return SQL_NO_DATA;
            }
            break;
        case SQL_FETCH_RELATIVE:
            new_index = current_index + fetch_offset;
            break;
        case SQL_FETCH_BOOKMARK: {
            if (!fetch_bookmark_ptr_) {
                setError("HY024", 0, "Fetch bookmark pointer not set");
                return SQL_ERROR;
            }
            SQLLEN bookmark_index = static_cast<SQLLEN>(*fetch_bookmark_ptr_);
            if (bookmark_index <= 0) {
                return SQL_NO_DATA;
            }
            new_index = static_cast<int64_t>(bookmark_index - 1) + fetch_offset;
            break;
        }
        default:
            setError("HY106", 0, "Fetch type out of range");
            return SQL_ERROR;
    }

    if (new_index < 0 || static_cast<size_t>(new_index) >= rows_.size()) {
        return SQL_NO_DATA;
    }

    current_row_ = static_cast<size_t>(new_index) + 1;
    clearGetDataState();
    auto result = bindResultData();

    return result;
}

void OdbcStatement::clearPutDataState() {
    put_data_stream_.clear();
    data_at_exec_params_.clear();
    data_at_exec_index_ = 0;
    data_at_exec_active_ = false;
    data_at_exec_row_offset_ = 0;
}

bool OdbcStatement::isDataAtExecIndicator(SQLLEN indicator) const {
    return isDataAtExecIndicatorValue(indicator);
}

SQLRETURN OdbcStatement::validateOrInitDataAtExecState() {
    return validateOrInitDataAtExecStateForRow(0);
}

SQLRETURN OdbcStatement::validateOrInitDataAtExecStateForRow(SQLULEN row_offset) {
    if (!prepared_) {
        setError("HY010", 0, "Function sequence error");
        return SQL_ERROR;
    }

    if (data_at_exec_active_ && !data_at_exec_params_.empty() &&
        data_at_exec_index_ < data_at_exec_params_.size() &&
        data_at_exec_row_offset_ == row_offset) {
        data_at_exec_active_ = true;
        return SQL_NEED_DATA;
    }

    data_at_exec_active_ = false;
    data_at_exec_params_.clear();
    data_at_exec_index_ = 0;
    data_at_exec_row_offset_ = row_offset;

    std::vector<SQLUSMALLINT> pending_params;
    pending_params.reserve(param_bindings_.size());

    for (const auto& [parameter_number, binding] : param_bindings_) {
        auto* ind = indicatorForRow(binding, row_offset);
        if (!ind || !isDataAtExecIndicator(*ind)) {
            continue;
        }

        auto key = putDataStreamKey(parameter_number, row_offset);
        auto stream_it = put_data_stream_.find(key);
        if (stream_it != put_data_stream_.end() && stream_it->second.complete) {
            continue;
        }

        auto expected = SQLLEN{0};
        bool expected_known = false;
        if (parseDataAtExecLength(*ind, &expected)) {
            expected_known = true;
        }

        if (stream_it == put_data_stream_.end()) {
            PutDataStreamState state;
            state.expected_length = expected;
            state.expected_length_known = expected_known;
            put_data_stream_.emplace(key, state);
        } else if (expected_known) {
            auto& state = stream_it->second;
            state.expected_length = expected;
            state.expected_length_known = true;
        }

        if (stream_it == put_data_stream_.end()) {
            // If insertion created a new stream iterator above, avoid another lookup
            // in the next condition by checking completion again with a fresh lookup.
            stream_it = put_data_stream_.find(key);
        }
        if (stream_it != put_data_stream_.end() && !stream_it->second.complete) {
            pending_params.push_back(parameter_number);
        }
    }

    if (!pending_params.empty()) {
        std::sort(pending_params.begin(), pending_params.end());
        data_at_exec_params_ = std::move(pending_params);
        data_at_exec_active_ = true;
        return SQL_NEED_DATA;
    }

    return SQL_SUCCESS;
}

SQLUSMALLINT OdbcStatement::getCurrentDataAtExecParameter() const {
    if (!data_at_exec_active_ || data_at_exec_params_.empty()) {
        return 0;
    }
    if (data_at_exec_index_ >= data_at_exec_params_.size()) {
        return 0;
    }
    return data_at_exec_params_[data_at_exec_index_];
}

SQLPOINTER OdbcStatement::putDataTokenToPointer(SQLUSMALLINT parameter_number) const {
    if (parameter_number == 0) {
        return nullptr;
    }
    return reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(parameter_number));
}

SQLUSMALLINT OdbcStatement::pointerToPutDataToken(SQLPOINTER token) const {
    auto raw = reinterpret_cast<uintptr_t>(token);
    if (raw == 0) {
        return 0;
    }
    return static_cast<SQLUSMALLINT>(raw);
}

uint64_t OdbcStatement::putDataStreamKey(SQLUSMALLINT parameter_number,
                                         SQLULEN row_offset) const {
    return (static_cast<uint64_t>(row_offset) << 16) |
           static_cast<uint64_t>(parameter_number);
}

const SQLLEN* OdbcStatement::indicatorForRow(const ParameterBinding& binding,
                                             SQLULEN row_offset) const {
    auto* ind_base = binding.str_len_or_ind;
    if (!ind_base) {
        return nullptr;
    }

    SQLULEN indicator_stride = 0;
    if (param_bind_type_ != 0) {
        indicator_stride = param_bind_type_;
    } else if (param_bind_offset_ > 0) {
        indicator_stride = static_cast<SQLULEN>(param_bind_offset_);
    } else {
        indicator_stride = static_cast<SQLULEN>(sizeof(SQLLEN));
    }

    SQLULEN indicator_base_offset = 0;
    if (param_bind_type_ != 0 && param_bind_offset_ > 0) {
        indicator_base_offset = static_cast<SQLULEN>(param_bind_offset_);
    }

    return reinterpret_cast<const SQLLEN*>(
        reinterpret_cast<const char*>(ind_base) +
        static_cast<size_t>(indicator_base_offset) +
        static_cast<size_t>(row_offset) * static_cast<size_t>(indicator_stride));
}

SQLRETURN OdbcStatement::paramData(SQLPOINTER* token) {
    clearDiagnostics();

    if (!token) {
        setError("HY009", 0, "Invalid use of null pointer");
        return SQL_ERROR;
    }

    if (!prepared_) {
        setError("HY010", 0, "Function sequence error");
        return SQL_ERROR;
    }

    if (!data_at_exec_active_) {
        auto status = validateOrInitDataAtExecState();
        if (status == SQL_SUCCESS) {
            *token = nullptr;
            return SQL_SUCCESS;
        }
        if (status == SQL_NEED_DATA) {
            auto current_param = getCurrentDataAtExecParameter();
            if (current_param == 0) {
                setError("HY000", 0, "Failed to initialize SQLDataAtExec state");
                return SQL_ERROR;
            }
            *token = putDataTokenToPointer(current_param);
            return SQL_NEED_DATA;
        }
        return status;
    }

    auto current_param = getCurrentDataAtExecParameter();
    if (current_param == 0) {
        setError("HY010", 0, "Function sequence error");
        return SQL_ERROR;
    }

    *token = putDataTokenToPointer(current_param);
    return SQL_NEED_DATA;
}

SQLRETURN OdbcStatement::putData(SQLPOINTER data, SQLLEN len) {
    clearDiagnostics();

    if (!prepared_) {
        setError("HY010", 0, "Function sequence error");
        return SQL_ERROR;
    }

    if (!data_at_exec_active_) {
        setError("HY010", 0, "Function sequence error");
        return SQL_ERROR;
    }

    auto current_param = getCurrentDataAtExecParameter();
    if (current_param == 0) {
        setError("HY010", 0, "Function sequence error");
        return SQL_ERROR;
    }

    auto binding_it = param_bindings_.find(current_param);
    if (binding_it == param_bindings_.end()) {
        setError("07009", 0, "Invalid descriptor index");
        return SQL_ERROR;
    }

    auto ind_ptr = indicatorForRow(binding_it->second, data_at_exec_row_offset_);
    if (!ind_ptr) {
        setError("HY000", 0, "Missing parameter indicator for SQL_DATA_AT_EXEC");
        return SQL_ERROR;
    }
    if (!isDataAtExecIndicator(*ind_ptr)) {
        setError("HY000", 0, "Parameter is not marked SQL_DATA_AT_EXEC");
        return SQL_ERROR;
    }

    auto stream_it = put_data_stream_.find(
        putDataStreamKey(current_param, data_at_exec_row_offset_));
    if (stream_it == put_data_stream_.end()) {
        setError("HY000", 0, "Invalid SQL_DATA_AT_EXEC parameter state");
        return SQL_ERROR;
    }

    auto& stream = stream_it->second;
    if (stream.complete) {
        setError("HY010", 0, "Function sequence error");
        return SQL_ERROR;
    }

    std::string chunk;
    if (len == SQL_NTS && binding_it->second.value_type == SQL_C_CHAR) {
        if (!data) {
            setError("HY009", 0, "Invalid use of null pointer");
            return SQL_ERROR;
        }
        chunk.assign(static_cast<const char*>(data), std::strlen(static_cast<const char*>(data)));
    } else if (len > 0) {
        if (!data) {
            setError("HY009", 0, "Invalid use of null pointer");
            return SQL_ERROR;
        }
        const auto* bytes = static_cast<const char*>(data);
        chunk.assign(bytes, bytes + len);
    } else if (len == 0) {
        chunk.clear();
    } else {
        setError("HY009", 0, "Invalid use of negative length");
        return SQL_ERROR;
    }

    if (stream.expected_length_known) {
        auto remaining = stream.expected_length - static_cast<SQLLEN>(stream.value.size());
        if (remaining <= 0) {
            stream.complete = true;
        } else if (static_cast<SQLLEN>(chunk.size()) > remaining) {
            stream.value.append(chunk.data(), static_cast<size_t>(remaining));
            stream.truncated = true;
            stream.complete = true;
            if (!chunk.empty()) {
                setError("01004", 0, "String data, right truncated");
            }
            if (data_at_exec_index_ + 1 < data_at_exec_params_.size()) {
                data_at_exec_index_++;
            } else {
                data_at_exec_active_ = false;
            }
            return SQL_SUCCESS_WITH_INFO;
        } else {
            stream.value.append(chunk);
        }

        if (static_cast<SQLLEN>(stream.value.size()) >= stream.expected_length) {
            stream.complete = true;
            if (data_at_exec_index_ + 1 < data_at_exec_params_.size()) {
                data_at_exec_index_++;
            } else {
                data_at_exec_active_ = false;
            }
        }
        return SQL_SUCCESS;
    }

    stream.value.append(chunk);
    if (len == 0) {
        stream.complete = true;
        if (data_at_exec_index_ + 1 < data_at_exec_params_.size()) {
            data_at_exec_index_++;
            return SQL_SUCCESS;
        }
        data_at_exec_active_ = false;
    }

    return SQL_SUCCESS;
}

void OdbcStatement::clearGetDataState() {
    get_data_stream_.clear();
}

SQLRETURN OdbcStatement::getData(SQLUSMALLINT column_number,
                                 SQLSMALLINT target_type,
                                 SQLPOINTER target_value,
                                 SQLLEN buffer_length,
                                 SQLLEN* str_len_or_ind) {
    return getDataInternal(column_number, target_type, target_value, buffer_length,
                           str_len_or_ind, true);
}

SQLRETURN OdbcStatement::getDataInternal(SQLUSMALLINT column_number,
                                         SQLSMALLINT target_type,
                                         SQLPOINTER target_value,
                                         SQLLEN buffer_length,
                                         SQLLEN* str_len_or_ind,
                                         bool stream_chunks) {
    clearDiagnostics();

    if (!has_results_) {
        setError("24000", 0, "Invalid cursor state");
        return SQL_ERROR;
    }

    if (current_row_ == 0 || current_row_ > rows_.size()) {
        setError("HY109", 0, "Invalid cursor position");
        return SQL_ERROR;
    }

    if (column_number == 0 || column_number > columns_.size()) {
        setError("07009", 0, "Invalid descriptor index");
        return SQL_ERROR;
    }

    const auto& value = rows_[current_row_ - 1][column_number - 1];
    const auto& column_meta = columns_[column_number - 1];
    bool is_binary_column = isBinarySqlType(column_meta.sql_type);
    bool stream_binary = target_type == SQL_C_BINARY;
    bool stream_text = (target_type == SQL_C_CHAR || target_type == SQL_C_DEFAULT);
    bool can_stream = stream_binary || stream_text;

    auto prepareStreamState = [&](SQLUSMALLINT column) -> GetDataStreamState& {
        auto it = get_data_stream_.find(column);
        if (it == get_data_stream_.end()) {
            GetDataStreamState state;
            if (stream_binary) {
                state.value = value;
            } else if (stream_text) {
                state.value = is_binary_column ? bytesToHexString(value) : value;
            }
            it = get_data_stream_.emplace(column, std::move(state)).first;
        }
        return it->second;
    };

    auto streamChunk = [&](SQLUSMALLINT column, bool is_text, SQLLEN* remaining_len_ptr) -> SQLRETURN {
        if (!can_stream) {
            return SQL_ERROR;
        }

        GetDataStreamState& stream_state = prepareStreamState(column);
        const auto& source = stream_state.value;
        const auto total_size = source.size();
        const auto offset = stream_state.offset;
        const auto new_offset = std::min(offset, total_size);

        if (new_offset >= total_size) {
            if (remaining_len_ptr) {
                *remaining_len_ptr = 0;
            }
            get_data_stream_.erase(column);
            return SQL_SUCCESS;
        }

        if (buffer_length <= 0) {
            if (remaining_len_ptr) {
                *remaining_len_ptr = static_cast<SQLLEN>(total_size);
            }
            return SQL_SUCCESS;
        }

        auto capacity = static_cast<size_t>(buffer_length);
        if (is_text) {
            capacity = std::max<size_t>(1, static_cast<size_t>(buffer_length));
            capacity -= 1;
        }

        const size_t remaining = total_size - new_offset;
        const size_t copy_size = std::min(capacity, remaining);
        if (remaining_len_ptr) {
            if (is_text) {
                if (!stream_chunks || stream_state.offset == 0) {
                    *remaining_len_ptr = static_cast<SQLLEN>(total_size);
                } else {
                    *remaining_len_ptr = (copy_size < remaining) ? static_cast<SQLLEN>(total_size) : 0;
                }
            } else {
                *remaining_len_ptr = static_cast<SQLLEN>(total_size);
            }
        }

        if (target_value && copy_size > 0) {
            std::memcpy(target_value,
                        source.data() + offset,
                        copy_size);
            if (is_text) {
                static_cast<char*>(target_value)[copy_size] = '\0';
            }
        } else if (is_text && target_value) {
            static_cast<char*>(target_value)[0] = '\0';
        }

        stream_state.offset += copy_size;
        if (stream_state.offset < total_size) {
            setError("01004", 0, "String data, right truncated");
            return SQL_SUCCESS_WITH_INFO;
        }
        return SQL_SUCCESS;
    };

    // Handle NULL
    if (value.empty()) {
        if (str_len_or_ind) {
            *str_len_or_ind = SQL_NULL_DATA;
        }
        return SQL_SUCCESS;
    }

    // Convert and store based on target type
    SQLRETURN result = SQL_SUCCESS;

    switch (target_type) {
        case SQL_C_CHAR:
        case SQL_C_DEFAULT: {
            SQLLEN total_length = 0;
            auto stream_result = streamChunk(column_number, true, &total_length);
            if (str_len_or_ind) {
                *str_len_or_ind = total_length;
            }
            return stream_result;
        }

        case SQL_C_LONG:
        case SQL_C_SLONG: {
            if (target_value) {
                *static_cast<SQLINTEGER*>(target_value) = std::stoi(value);
            }
            if (str_len_or_ind) {
                *str_len_or_ind = sizeof(SQLINTEGER);
            }
            break;
        }

        case SQL_C_SHORT:
        case SQL_C_SSHORT: {
            if (target_value) {
                *static_cast<SQLSMALLINT*>(target_value) = static_cast<SQLSMALLINT>(std::stoi(value));
            }
            if (str_len_or_ind) {
                *str_len_or_ind = sizeof(SQLSMALLINT);
            }
            break;
        }

        case SQL_C_SBIGINT: {
            if (target_value) {
                *static_cast<int64_t*>(target_value) = std::stoll(value);
            }
            if (str_len_or_ind) {
                *str_len_or_ind = sizeof(int64_t);
            }
            break;
        }

        case SQL_C_DOUBLE: {
            if (target_value) {
                *static_cast<SQLDOUBLE*>(target_value) = std::stod(value);
            }
            if (str_len_or_ind) {
                *str_len_or_ind = sizeof(SQLDOUBLE);
            }
            break;
        }

        case SQL_C_FLOAT: {
            if (target_value) {
                *static_cast<SQLREAL*>(target_value) = std::stof(value);
            }
            if (str_len_or_ind) {
                *str_len_or_ind = sizeof(SQLREAL);
            }
            break;
        }

        case SQL_C_BIT: {
            if (target_value) {
                *static_cast<unsigned char*>(target_value) =
                    (value == "1" || value == "true" || value == "t") ? 1 : 0;
            }
            if (str_len_or_ind) {
                *str_len_or_ind = 1;
            }
            break;
        }

        case SQL_C_BINARY: {
            SQLLEN total_length = 0;
            auto stream_result = streamChunk(column_number, false, &total_length);
            if (str_len_or_ind) {
                *str_len_or_ind = total_length;
            }
            return stream_result;
        }

        case SQL_C_DATE: {
            if (!target_value) {
                setError("HY009", 0, "Invalid use of null pointer");
                return SQL_ERROR;
            }
            SQL_DATE_STRUCT date{};
            if (!parseDateLiteral(value, date)) {
                setError("22007", 0, "Invalid datetime format");
                return SQL_ERROR;
            }
            *static_cast<SQL_DATE_STRUCT*>(target_value) = date;
            if (str_len_or_ind) {
                *str_len_or_ind = sizeof(SQL_DATE_STRUCT);
            }
            break;
        }

        case SQL_C_TIME: {
            if (!target_value) {
                setError("HY009", 0, "Invalid use of null pointer");
                return SQL_ERROR;
            }
            SQL_TIME_STRUCT time{};
            if (!parseTimeLiteral(value, time)) {
                setError("22007", 0, "Invalid datetime format");
                return SQL_ERROR;
            }
            *static_cast<SQL_TIME_STRUCT*>(target_value) = time;
            if (str_len_or_ind) {
                *str_len_or_ind = sizeof(SQL_TIME_STRUCT);
            }
            break;
        }

        case SQL_C_TIMESTAMP: {
            if (!target_value) {
                setError("HY009", 0, "Invalid use of null pointer");
                return SQL_ERROR;
            }
            SQL_TIMESTAMP_STRUCT ts{};
            if (!parseTimestampLiteral(value, ts)) {
                setError("22007", 0, "Invalid datetime format");
                return SQL_ERROR;
            }
            *static_cast<SQL_TIMESTAMP_STRUCT*>(target_value) = ts;
            if (str_len_or_ind) {
                *str_len_or_ind = sizeof(SQL_TIMESTAMP_STRUCT);
            }
            break;
        }

        case SQL_C_GUID: {
            if (!target_value) {
                setError("HY009", 0, "Invalid use of null pointer");
                return SQL_ERROR;
            }
            SQLGUID guid{};
            if (value.size() == 16) {
                std::string hex = bytesToHexString(value);
                if (!parseGuidString(hex, guid)) {
                    setError("22018", 0, "Invalid GUID format");
                    return SQL_ERROR;
                }
            } else if (!parseGuidString(value, guid)) {
                setError("22018", 0, "Invalid GUID format");
                return SQL_ERROR;
            }
            *static_cast<SQLGUID*>(target_value) = guid;
            if (str_len_or_ind) {
                *str_len_or_ind = sizeof(SQLGUID);
            }
            break;
        }

        default:
            setError("HY003", 0, "Program type out of range");
            return SQL_ERROR;
    }

    return result;
}

SQLRETURN OdbcStatement::rowCount(SQLLEN* row_count_ptr) {
    clearDiagnostics();

    if (!row_count_ptr) {
        setError("HY009", 0, "Invalid use of null pointer");
        return SQL_ERROR;
    }

    *row_count_ptr = row_count_;
    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::moreResults() {
    clearDiagnostics();
    if (result_sets_.empty()) {
        return SQL_NO_DATA;
    }

    size_t next_index = current_result_index_ + 1;
    if (next_index >= result_sets_.size()) {
        return SQL_NO_DATA;
    }

    current_result_index_ = next_index;
    applyResultSet(current_result_index_);
    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::setPos(SQLSETPOSIROW row_number, SQLUSMALLINT operation,
                                 SQLUSMALLINT lock_type) {
    clearDiagnostics();

    if (!has_results_ || rows_.empty()) {
        setError("24000", 0, "Invalid cursor state");
        return SQL_ERROR;
    }

    if (cursor_type_ == SQL_CURSOR_FORWARD_ONLY) {
        setError("HY106", 0, "Forward-only cursor does not support positioned operations");
        return SQL_ERROR;
    }

    switch (lock_type) {
        case SQL_LOCK_NO_CHANGE:
        case SQL_LOCK_EXCLUSIVE:
        case SQL_LOCK_UNLOCK:
            break;
        default:
            setError("HY024", 0, "Invalid lock type");
            return SQL_ERROR;
    }

    auto setStatusForAllRows = [&](SQLUSMALLINT status) {
        if (!row_status_ptr_) {
            return;
        }
        row_status_ptr_[0] = status;
    };

    auto setStatusForEntireRowset = [&](SQLUSMALLINT status) {
        if (!row_status_ptr_) {
            return;
        }
        const SQLULEN status_count = row_array_size_ > 0 ? row_array_size_ : 1;
        for (SQLULEN i = 0; i < status_count; ++i) {
            row_status_ptr_[i] = status;
        }
    };

    auto setStatusForRow = [&](SQLSETPOSIROW row, SQLUSMALLINT status) {
        setStatusForAllRows(status);
        if (!row_status_ptr_ || row <= 0) {
            return;
        }
        size_t zero_based_row = static_cast<size_t>(row) - 1;
        if (zero_based_row >= rows_.size()) {
            return;
        }
        if (row_array_size_ == 0 || zero_based_row < row_array_size_) {
            row_status_ptr_[zero_based_row] = status;
        }
    };

    auto uses_entire_rowset = [](SQLSETPOSIROW row) { return row == SQL_ENTIRE_ROWSET; };
    auto valid_row = [&](SQLSETPOSIROW row) {
        return row >= 1 && static_cast<size_t>(row) <= rows_.size();
    };

    clearGetDataState();

    SQLLEN affected_count = 0;
    switch (operation) {
        case SQL_POSITION: {
            if (uses_entire_rowset(row_number)) {
                setError("HY109", 0, "Invalid cursor position");
                return SQL_ERROR;
            }
            if (!valid_row(row_number)) {
                setError("HY109", 0, "Invalid cursor position");
                return SQL_ERROR;
            }
            current_row_ = static_cast<size_t>(row_number);
            setStatusForRow(row_number, SQL_ROW_SUCCESS);
            break;
        }

        case SQL_REFRESH: {
            if (uses_entire_rowset(row_number)) {
                setError("HY109", 0, "Invalid cursor position");
                return SQL_ERROR;
            }
            if (!valid_row(row_number)) {
                setError("HY109", 0, "Invalid cursor position");
                return SQL_ERROR;
            }
            // Rebind and return current row as unchanged.
            current_row_ = static_cast<size_t>(row_number);
            auto bind_result = bindResultData();
            if (bind_result != SQL_SUCCESS && bind_result != SQL_SUCCESS_WITH_INFO) {
                return bind_result;
            }
            setStatusForRow(row_number, SQL_ROW_SUCCESS);
            affected_count = 1;
            break;
        }

        case SQL_UPDATE: {
            if (uses_entire_rowset(row_number)) {
                setError("HY109", 0, "Invalid cursor position");
                return SQL_ERROR;
            }
            if (concurrency_ == SQL_CONCUR_READ_ONLY) {
                setError("25001", 0, "Read-only cursor does not support update");
                return SQL_ERROR;
            }
            if (!valid_row(row_number)) {
                setError("HY109", 0, "Invalid cursor position");
                return SQL_ERROR;
            }
            current_row_ = static_cast<size_t>(row_number);
            setStatusForRow(row_number, SQL_ROW_UPDATED);
            affected_count = 1;
            break;
        }

        case SQL_DELETE: {
            if (concurrency_ == SQL_CONCUR_READ_ONLY) {
                setError("25001", 0, "Read-only cursor does not support delete");
                return SQL_ERROR;
            }
            if (uses_entire_rowset(row_number)) {
                affected_count = static_cast<SQLLEN>(rows_.size());
                row_count_ = affected_count;
                rows_.clear();
                current_row_ = 0;
                setStatusForEntireRowset(SQL_ROW_DELETED);
                break;
            }
            if (!valid_row(row_number)) {
                setError("HY109", 0, "Invalid cursor position");
                return SQL_ERROR;
            }
            rows_.erase(rows_.begin() + static_cast<size_t>(row_number - 1));
            setStatusForRow(row_number, SQL_ROW_DELETED);
            affected_count = 1;

            if (rows_.empty()) {
                current_row_ = 0;
            } else if (static_cast<size_t>(row_number) > rows_.size()) {
                current_row_ = rows_.size();
            } else {
                current_row_ = static_cast<size_t>(row_number);
            }
            break;
        }

        default:
            setError("HY092", 0, "Invalid SQLSetPos operation");
            return SQL_ERROR;
    }

    if (affected_count > 0) {
        row_count_ = static_cast<SQLLEN>(affected_count);
    }
    if (rows_fetched_ptr_) {
        *rows_fetched_ptr_ = affected_count > 0 ? static_cast<SQLULEN>(affected_count) : 1;
    }

    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::bulkOperations(SQLSMALLINT operation) {
    clearDiagnostics();
    auto finish = [&](SQLRETURN rc) -> SQLRETURN {
        if (rc != SQL_NEED_DATA) {
            clearPutDataState();
        }
        return rc;
    };

    bool supported_operation = (operation == SQL_ADD ||
                                operation == SQL_UPDATE_BY_BOOKMARK ||
                                operation == SQL_DELETE_BY_BOOKMARK);
#ifdef SQL_FETCH_BY_BOOKMARK
    supported_operation = supported_operation || (operation == SQL_FETCH_BY_BOOKMARK);
#endif
    if (!supported_operation) {
        setError("HY092", 0, "Invalid SQLBulkOperations operation");
        return finish(SQL_ERROR);
    }

#ifdef SQL_FETCH_BY_BOOKMARK
    if (operation == SQL_FETCH_BY_BOOKMARK) {
        if (!has_results_ || rows_.empty()) {
            setError("24000", 0, "Invalid cursor state");
            return finish(SQL_ERROR);
        }
        if (!fetch_bookmark_ptr_) {
            setError("HY024", 0, "Fetch bookmark pointer not set");
            return finish(SQL_ERROR);
        }
        SQLLEN bookmark_index = static_cast<SQLLEN>(*fetch_bookmark_ptr_);
        if (bookmark_index <= 0) {
            setError("HY109", 0, "Invalid cursor position");
            return finish(SQL_ERROR);
        }
        if (static_cast<size_t>(bookmark_index) > rows_.size()) {
            if (row_status_ptr_) {
                row_status_ptr_[0] = SQL_ROW_NOROW;
            }
            if (rows_fetched_ptr_) {
                *rows_fetched_ptr_ = 0;
            }
            if (params_processed_ptr_) {
                *params_processed_ptr_ = 0;
            }
            return finish(SQL_NO_DATA);
        }

        current_row_ = static_cast<size_t>(bookmark_index);
        SQLRETURN bind_rc = bindResultData();
        if (bind_rc != SQL_SUCCESS && bind_rc != SQL_SUCCESS_WITH_INFO) {
            return finish(bind_rc);
        }
        row_count_ = 1;
        if (params_processed_ptr_) {
            *params_processed_ptr_ = 0;
        }
        return finish(bind_rc);
    }
#endif

    if (!prepared_) {
        setError("HY010", 0, "Function sequence error");
        return finish(SQL_ERROR);
    }

    if (paramset_size_ == 0) {
        if (params_processed_ptr_) {
            *params_processed_ptr_ = 0;
        }
        if (rows_fetched_ptr_) {
            *rows_fetched_ptr_ = 0;
        }
        return finish(SQL_SUCCESS);
    }

    if (param_status_ptr_) {
        for (SQLULEN i = 0; i < paramset_size_; ++i) {
            param_status_ptr_[i] = 0;
        }
    }

    // Preflight all rows first so SQL_DATA_AT_EXEC is collected before any row executes.
    for (SQLULEN row = 0; row < paramset_size_; ++row) {
        std::vector<ParameterLiteral> params;
        auto build_status = buildParameterData(params, row);
        if (build_status == SQL_NEED_DATA) {
            if (params_processed_ptr_) {
                *params_processed_ptr_ = 0;
            }
            if (rows_fetched_ptr_) {
                *rows_fetched_ptr_ = 0;
            }
            return finish(SQL_NEED_DATA);
        }
        if (build_status != SQL_SUCCESS && build_status != SQL_SUCCESS_WITH_INFO) {
            if (param_status_ptr_) {
                param_status_ptr_[row] = SQL_PARAM_ERROR;
            }
            if (params_processed_ptr_) {
                *params_processed_ptr_ = 0;
            }
            if (rows_fetched_ptr_) {
                *rows_fetched_ptr_ = 0;
            }
            return finish(build_status);
        }
    }

    bool info_seen = false;
    SQLULEN processed = 0;

    for (SQLULEN row = 0; row < paramset_size_; ++row) {
        std::vector<ParameterLiteral> params;
        auto build_status = buildParameterData(params, row);
        if (build_status == SQL_NEED_DATA) {
            if (params_processed_ptr_) {
                *params_processed_ptr_ = processed;
            }
            if (rows_fetched_ptr_) {
                *rows_fetched_ptr_ = processed;
            }
            return finish(SQL_NEED_DATA);
        }
        const bool row_has_info = (build_status == SQL_SUCCESS_WITH_INFO);
        if (build_status != SQL_SUCCESS && build_status != SQL_SUCCESS_WITH_INFO) {
            if (param_status_ptr_) {
                param_status_ptr_[row] = SQL_PARAM_ERROR;
            }
            if (params_processed_ptr_) {
                *params_processed_ptr_ = processed;
            }
            if (rows_fetched_ptr_) {
                *rows_fetched_ptr_ = processed;
            }
            return finish(build_status);
        }
        if (build_status == SQL_SUCCESS_WITH_INFO) {
            info_seen = true;
        }

        std::string sql;
        auto build_sql_status = conn_->buildPreparedSQL(server_stmt_id_, params, sql);
        if (build_sql_status != SQL_SUCCESS) {
            if (param_status_ptr_) {
                param_status_ptr_[row] = SQL_PARAM_ERROR;
            }
            if (params_processed_ptr_) {
                *params_processed_ptr_ = processed;
            }
            if (rows_fetched_ptr_) {
                *rows_fetched_ptr_ = processed;
            }
            return finish(build_sql_status);
        }

        std::vector<std::vector<std::string>> rows;
        std::vector<ColumnMetadata> columns;
        SQLLEN rows_affected = 0;
        auto rc = conn_->executeSQL(sql, rows, columns, rows_affected);
        if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
            std::string primary_state{"HY000"};
            SQLINTEGER primary_native{0};
            std::string primary_message{"Bulk operation failed"};
            if (const auto* primary_diag = conn_->getDiagnostic(1)) {
                primary_state = primary_diag->sqlstate;
                primary_native = primary_diag->native_error;
                primary_message = primary_diag->message;
            }

            setError(primary_state, primary_native, primary_message);

            if (processed > 0) {
                // If the batch partially applied inside an explicit transaction, attempt rollback
                // to avoid leaving mixed-success state on the server.
                auto rollback_rc = conn_->endTransaction(SQL_ROLLBACK);
                if (rollback_rc != SQL_SUCCESS && rollback_rc != SQL_SUCCESS_WITH_INFO) {
                    DiagnosticRecord rollback_diag;
                    rollback_diag.sqlstate = "HY000";
                    rollback_diag.message = "Rollback after partial batch failure failed";
                    if (const auto* conn_rollback_diag = conn_->getDiagnostic(1)) {
                        rollback_diag.sqlstate = conn_rollback_diag->sqlstate;
                        rollback_diag.native_error = conn_rollback_diag->native_error;
                        rollback_diag.message =
                            "Rollback after partial batch failure failed: " + conn_rollback_diag->message;
                    }
                    addDiagnostic(rollback_diag);
                } else {
                    DiagnosticRecord rollback_diag;
                    rollback_diag.sqlstate = "01000";
                    rollback_diag.message = "Partially applied batch was rolled back";
                    addDiagnostic(rollback_diag);
                }
            }
            if (param_status_ptr_) {
                param_status_ptr_[row] = SQL_PARAM_ERROR;
            }
            if (params_processed_ptr_) {
                *params_processed_ptr_ = processed;
            }
            if (rows_fetched_ptr_) {
                *rows_fetched_ptr_ = processed;
            }
            return finish(rc);
        }
        if (param_status_ptr_) {
            if (row_has_info || rc == SQL_SUCCESS_WITH_INFO) {
                param_status_ptr_[row] = SQL_PARAM_SUCCESS_WITH_INFO;
                info_seen = true;
            } else {
                param_status_ptr_[row] = SQL_PARAM_SUCCESS;
            }
        }
        if (build_status == SQL_SUCCESS_WITH_INFO) {
            info_seen = true;
        }
        ++processed;
    }

    if (paramset_size_ > 0) {
        row_count_ = static_cast<SQLLEN>(processed);
    }
    if (params_processed_ptr_) {
        *params_processed_ptr_ = processed;
    }
    if (rows_fetched_ptr_) {
        *rows_fetched_ptr_ = processed;
    }

    return finish(info_seen ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS);
}

SQLRETURN OdbcStatement::setAttribute(SQLINTEGER attribute, SQLPOINTER value,
                                       SQLINTEGER /*string_length*/) {
    clearDiagnostics();

    auto resolveDescriptorHandle = [&](SQLPOINTER descriptor_handle,
                                      OdbcDescriptor::DescriptorType expected_type,
                                      OdbcDescriptor*& destination) -> SQLRETURN {
        if (!descriptor_handle) {
            setError("HY024", 0, "Invalid descriptor handle");
            return SQL_ERROR;
        }

        auto* candidate = asDescriptor(static_cast<SQLHDESC>(descriptor_handle));
        if (!candidate) {
            setError("HY024", 0, "Invalid descriptor handle");
            return SQL_ERROR;
        }

        if (candidate->getConnection() != conn_) {
            setError("HY024", 0, "Descriptor belongs to different connection");
            return SQL_ERROR;
        }

        if (candidate->getDescriptorType() != expected_type) {
            setError("HY024", 0, "Descriptor type mismatch");
            return SQL_ERROR;
        }

        destination = candidate;
        return SQL_SUCCESS;
    };

    switch (attribute) {
        case SQL_ATTR_CURSOR_TYPE:
            {
                auto new_cursor_type = ODBC_PTR_TO_ULEN(value);
                if (new_cursor_type != SQL_CURSOR_FORWARD_ONLY &&
                    new_cursor_type != SQL_CURSOR_KEYSET_DRIVEN &&
                    new_cursor_type != SQL_CURSOR_DYNAMIC &&
                    new_cursor_type != SQL_CURSOR_STATIC) {
                    setError("HY024", 0, "Invalid attribute value");
                    return SQL_ERROR;
                }
                cursor_type_ = new_cursor_type;
            }
            break;
        case SQL_ATTR_CONCURRENCY:
            {
                auto new_concurrency = ODBC_PTR_TO_ULEN(value);
                if (new_concurrency != SQL_CONCUR_READ_ONLY &&
                    new_concurrency != SQL_CONCUR_LOCK &&
                    new_concurrency != SQL_CONCUR_ROWVER &&
                    new_concurrency != SQL_CONCUR_VALUES) {
                    setError("HY024", 0, "Invalid attribute value");
                    return SQL_ERROR;
                }
                concurrency_ = new_concurrency;
            }
            break;
        case SQL_ATTR_QUERY_TIMEOUT:
            query_timeout_ = ODBC_PTR_TO_ULEN(value);
            break;
        case SQL_ATTR_MAX_ROWS:
            max_rows_ = ODBC_PTR_TO_ULEN(value);
            break;
        case SQL_ATTR_MAX_LENGTH:
            max_length_ = ODBC_PTR_TO_ULEN(value);
            break;
        case SQL_ATTR_ROW_ARRAY_SIZE:
            row_array_size_ = ODBC_PTR_TO_ULEN(value);
            break;
        case SQL_ATTR_ROWS_FETCHED_PTR:
            rows_fetched_ptr_ = static_cast<SQLULEN*>(value);
            break;
        case SQL_ATTR_ROW_STATUS_PTR:
            row_status_ptr_ = static_cast<SQLUSMALLINT*>(value);
            break;
        case SQL_ATTR_ROW_BIND_OFFSET_PTR:
            // value is pointer to SQLLEN
            if (value) row_bind_offset_ = *static_cast<SQLLEN*>(value);
            break;
        case SQL_ATTR_ROW_BIND_TYPE:
            row_bind_type_ = ODBC_PTR_TO_ULEN(value);
            break;
        case SQL_ATTR_PARAMSET_SIZE:
            paramset_size_ = ODBC_PTR_TO_ULEN(value);
            break;
        case SQL_ATTR_PARAMS_PROCESSED_PTR:
            params_processed_ptr_ = static_cast<SQLULEN*>(value);
            break;
        case SQL_ATTR_PARAM_STATUS_PTR:
            param_status_ptr_ = static_cast<SQLUSMALLINT*>(value);
            break;
        case SQL_ATTR_PARAM_BIND_OFFSET_PTR:
            if (value) param_bind_offset_ = *static_cast<SQLLEN*>(value);
            break;
        case SQL_ATTR_PARAM_BIND_TYPE:
            param_bind_type_ = ODBC_PTR_TO_ULEN(value);
            break;
        case SQL_ATTR_NOSCAN:
            noscan_ = (ODBC_PTR_TO_ULEN(value) != 0);
            break;
        case SQL_ATTR_USE_BOOKMARKS:
            use_bookmarks_ = (ODBC_PTR_TO_ULEN(value) != 0);
            break;
        case SQL_ATTR_FETCH_BOOKMARK_PTR:
            fetch_bookmark_ptr_ = static_cast<BOOKMARK*>(value);
            break;
        case SQL_ATTR_RETRIEVE_DATA:
            retrieve_data_ = (ODBC_PTR_TO_ULEN(value) != 0);
            break;
        case SQL_ATTR_CURSOR_SCROLLABLE:
            {
                auto cursor_scrollable = ODBC_PTR_TO_ULEN(value);
                if (cursor_scrollable != SQL_NONSCROLLABLE &&
                    cursor_scrollable != SQL_SCROLLABLE) {
                    setError("HY024", 0, "Invalid attribute value");
                    return SQL_ERROR;
                }
                cursor_scrollable_ = cursor_scrollable;
            }
            break;
        case SQL_ATTR_CURSOR_SENSITIVITY:
            {
                auto cursor_sensitivity = ODBC_PTR_TO_ULEN(value);
                if (cursor_sensitivity != SQL_UNSPECIFIED &&
                    cursor_sensitivity != SQL_SENSITIVE &&
                    cursor_sensitivity != SQL_INSENSITIVE) {
                    setError("HY024", 0, "Invalid attribute value");
                    return SQL_ERROR;
                }
                cursor_sensitivity_ = cursor_sensitivity;
            }
            break;
        case SQL_ATTR_APP_ROW_DESC:
            if (!value) {
                app_row_desc_ = owned_app_row_desc_.get();
            } else if (resolveDescriptorHandle(value, OdbcDescriptor::DescriptorType::ARD, app_row_desc_) != SQL_SUCCESS) {
                return SQL_ERROR;
            }
            break;
        case SQL_ATTR_APP_PARAM_DESC:
            if (!value) {
                app_param_desc_ = owned_app_param_desc_.get();
            } else if (resolveDescriptorHandle(value, OdbcDescriptor::DescriptorType::APD, app_param_desc_) != SQL_SUCCESS) {
                return SQL_ERROR;
            }
            break;
        case SQL_ATTR_IMP_ROW_DESC:
            if (!value) {
                ird_desc_ = owned_ird_desc_.get();
            } else if (resolveDescriptorHandle(value, OdbcDescriptor::DescriptorType::IRD, ird_desc_) != SQL_SUCCESS) {
                return SQL_ERROR;
            }
            break;
        case SQL_ATTR_IMP_PARAM_DESC:
            if (!value) {
                ipd_desc_ = owned_imp_param_desc_.get();
            } else if (resolveDescriptorHandle(value, OdbcDescriptor::DescriptorType::IPD, ipd_desc_) != SQL_SUCCESS) {
                return SQL_ERROR;
            }
            break;
        default:
            setError("HY092", 0, "Invalid attribute identifier");
            return SQL_ERROR;
    }

    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::getAttribute(SQLINTEGER attribute, SQLPOINTER value,
                                       SQLINTEGER /*buffer_length*/,
                                       SQLINTEGER* string_length) {
    clearDiagnostics();

    auto setLen = [&](size_t len) {
        if (string_length) *string_length = static_cast<SQLINTEGER>(len);
    };

    switch (attribute) {
        case SQL_ATTR_CURSOR_TYPE:
            if (value) *static_cast<SQLULEN*>(value) = cursor_type_;
            setLen(sizeof(SQLULEN));
            break;
        case SQL_ATTR_CONCURRENCY:
            if (value) *static_cast<SQLULEN*>(value) = concurrency_;
            setLen(sizeof(SQLULEN));
            break;
        case SQL_ATTR_QUERY_TIMEOUT:
            if (value) *static_cast<SQLULEN*>(value) = query_timeout_;
            setLen(sizeof(SQLULEN));
            break;
        case SQL_ATTR_MAX_ROWS:
            if (value) *static_cast<SQLULEN*>(value) = max_rows_;
            setLen(sizeof(SQLULEN));
            break;
        case SQL_ATTR_MAX_LENGTH:
            if (value) *static_cast<SQLULEN*>(value) = max_length_;
            setLen(sizeof(SQLULEN));
            break;
        case SQL_ATTR_USE_BOOKMARKS:
            if (value) *static_cast<SQLULEN*>(value) = use_bookmarks_ ? 1 : 0;
            setLen(sizeof(SQLULEN));
            break;
        case SQL_ATTR_FETCH_BOOKMARK_PTR:
            if (value) *static_cast<BOOKMARK**>(value) = fetch_bookmark_ptr_;
            setLen(sizeof(SQLPOINTER));
            break;
        case SQL_ATTR_CURSOR_SCROLLABLE:
            if (value) *static_cast<SQLULEN*>(value) = cursor_scrollable_;
            setLen(sizeof(SQLULEN));
            break;
        case SQL_ATTR_CURSOR_SENSITIVITY:
            if (value) *static_cast<SQLULEN*>(value) = cursor_sensitivity_;
            setLen(sizeof(SQLULEN));
            break;
        case SQL_ATTR_ROW_ARRAY_SIZE:
            if (value) *static_cast<SQLULEN*>(value) = row_array_size_;
            setLen(sizeof(SQLULEN));
            break;
        case SQL_ATTR_ROW_NUMBER:
            if (value) *static_cast<SQLULEN*>(value) = static_cast<SQLULEN>(current_row_);
            setLen(sizeof(SQLULEN));
            break;
        case SQL_ATTR_PARAMSET_SIZE:
            if (value) *static_cast<SQLULEN*>(value) = paramset_size_;
            setLen(sizeof(SQLULEN));
            break;
        case SQL_ATTR_IMP_ROW_DESC:
            if (!value) {
                setError("HY009", 0, "Invalid use of null pointer");
                return SQL_ERROR;
            }
            *static_cast<SQLHDESC*>(value) = ird_desc_;
            setLen(sizeof(SQLPOINTER));
            break;
        case SQL_ATTR_IMP_PARAM_DESC:
            if (!value) {
                setError("HY009", 0, "Invalid use of null pointer");
                return SQL_ERROR;
            }
            *static_cast<SQLHDESC*>(value) = ipd_desc_;
            setLen(sizeof(SQLPOINTER));
            break;
        case SQL_ATTR_APP_ROW_DESC:
            if (!value) {
                setError("HY009", 0, "Invalid use of null pointer");
                return SQL_ERROR;
            }
            *static_cast<SQLHDESC*>(value) = app_row_desc_;
            setLen(sizeof(SQLPOINTER));
            break;
        case SQL_ATTR_APP_PARAM_DESC:
            if (!value) {
                setError("HY009", 0, "Invalid use of null pointer");
                return SQL_ERROR;
            }
            *static_cast<SQLHDESC*>(value) = app_param_desc_;
            setLen(sizeof(SQLPOINTER));
            break;
        default:
            setError("HY092", 0, "Invalid attribute identifier");
            return SQL_ERROR;
    }

    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::bindResultData() {
    if (current_row_ == 0 || current_row_ > rows_.size()) {
        return SQL_NO_DATA;
    }

    const auto& row = rows_[current_row_ - 1];
    SQLRETURN result = SQL_SUCCESS;

    if (bookmark_bound_) {
        SQLPOINTER bookmark_target = bookmark_binding_.target_value;
        SQLLEN* bookmark_ind = bookmark_binding_.str_len_or_ind;
        if (row_bind_offset_ != 0 && bookmark_target) {
            bookmark_target = static_cast<char*>(bookmark_target) + row_bind_offset_;
            if (bookmark_ind) {
                bookmark_ind = reinterpret_cast<SQLLEN*>(
                    reinterpret_cast<char*>(bookmark_ind) + row_bind_offset_);
            }
        }

        BOOKMARK bookmark_value = static_cast<BOOKMARK>(current_row_);
        switch (bookmark_binding_.target_type) {
            case SQL_C_SBIGINT:
            case SQL_C_UBIGINT:
                if (bookmark_target) {
                    *static_cast<BOOKMARK*>(bookmark_target) = bookmark_value;
                }
                if (bookmark_ind) {
                    *bookmark_ind = static_cast<SQLLEN>(sizeof(BOOKMARK));
                }
                break;
            case SQL_C_LONG:
            case SQL_C_SLONG:
            case SQL_C_ULONG:
                if (bookmark_target) {
                    *static_cast<SQLINTEGER*>(bookmark_target) = static_cast<SQLINTEGER>(bookmark_value);
                }
                if (bookmark_ind) {
                    *bookmark_ind = static_cast<SQLLEN>(sizeof(SQLINTEGER));
                }
                break;
            case SQL_C_CHAR:
            case SQL_C_DEFAULT: {
                std::string bookmark_text = std::to_string(bookmark_value);
                if (bookmark_target && bookmark_binding_.buffer_length > 0) {
                    size_t copy_len = std::min(
                        static_cast<size_t>(bookmark_binding_.buffer_length - 1),
                        bookmark_text.size());
                    std::memcpy(bookmark_target, bookmark_text.data(), copy_len);
                    static_cast<char*>(bookmark_target)[copy_len] = '\0';
                    if (bookmark_ind) {
                        *bookmark_ind = static_cast<SQLLEN>(bookmark_text.size());
                    }
                    if (bookmark_text.size() >= static_cast<size_t>(bookmark_binding_.buffer_length)) {
                        setError("01004", 0, "String data, right truncated");
                        result = SQL_SUCCESS_WITH_INFO;
                    }
                } else if (bookmark_ind) {
                    *bookmark_ind = static_cast<SQLLEN>(bookmark_text.size());
                }
                break;
            }
            default:
                setError("HY003", 0, "Program type out of range for bookmark");
                return SQL_ERROR;
        }
    }

    for (const auto& [col_num, binding] : col_bindings_) {
        if (col_num == 0) {
            continue;
        }
        if (col_num > row.size()) continue;

        const auto& value = row[col_num - 1];
        SQLLEN* str_len_or_ind = binding.str_len_or_ind;
        SQLPOINTER target = binding.target_value;
        SQLLEN buffer_len = binding.buffer_length;

        // Apply row offset if using row-wise binding
        if (row_bind_offset_ != 0 && target) {
            target = static_cast<char*>(target) + row_bind_offset_;
            if (str_len_or_ind) {
                str_len_or_ind = reinterpret_cast<SQLLEN*>(
                    reinterpret_cast<char*>(str_len_or_ind) + row_bind_offset_);
            }
        }

        // Handle NULL
        if (value.empty()) {
            if (str_len_or_ind) *str_len_or_ind = SQL_NULL_DATA;
            continue;
        }

        // Convert and store
        auto conv_result = getDataInternal(col_num,
                                          binding.target_type,
                                          target,
                                          buffer_len,
                                          str_len_or_ind,
                                          false);
        if (conv_result == SQL_SUCCESS_WITH_INFO) {
            result = SQL_SUCCESS_WITH_INFO;
        } else if (conv_result == SQL_ERROR) {
            return SQL_ERROR;
        }
    }

    // Set row status
    if (row_status_ptr_) {
        row_status_ptr_[0] = SQL_ROW_SUCCESS;
    }
    if (rows_fetched_ptr_) {
        *rows_fetched_ptr_ = 1;
    }

    return result;
}

SQLRETURN OdbcStatement::convertAndStore(size_t /*col_index*/, const std::string& /*value*/) {
    // Helper for data conversion - implemented in getData
    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::buildParameterData(std::vector<ParameterLiteral>& literals,
                                           SQLULEN row_offset) {
    literals.clear();
    literals.reserve(param_bindings_.size());

    bool has_info = false;
    auto bytesPerValue = [](const ParameterBinding& binding) -> SQLLEN {
        if (binding.buffer_length > 0) {
            return binding.buffer_length;
        }
        switch (binding.value_type) {
            case SQL_C_CHAR:
            case SQL_C_WCHAR:
                return 1;
            case SQL_C_SHORT:
            case SQL_C_SSHORT:
            case SQL_C_USHORT:
                return static_cast<SQLLEN>(sizeof(SQLSMALLINT));
            case SQL_C_LONG:
            case SQL_C_SLONG:
            case SQL_C_ULONG:
                return static_cast<SQLLEN>(sizeof(SQLINTEGER));
            case SQL_C_SBIGINT:
            case SQL_C_UBIGINT:
                return static_cast<SQLLEN>(sizeof(SQLBIGINT));
            case SQL_C_FLOAT:
                return static_cast<SQLLEN>(sizeof(SQLREAL));
            case SQL_C_DOUBLE:
                return static_cast<SQLLEN>(sizeof(SQLDOUBLE));
            case SQL_C_BIT:
                return static_cast<SQLLEN>(sizeof(SQLCHAR));
            case SQL_C_BINARY:
            case SQL_C_DATE:
                return static_cast<SQLLEN>(sizeof(SQL_DATE_STRUCT));
            case SQL_C_TIME:
                return static_cast<SQLLEN>(sizeof(SQL_TIME_STRUCT));
            case SQL_C_TIMESTAMP:
                return static_cast<SQLLEN>(sizeof(SQL_TIMESTAMP_STRUCT));
            case SQL_C_GUID:
                return static_cast<SQLLEN>(sizeof(SQLGUID));
            default:
                return 1;
        }
    };

    auto rowStride = [&](const ParameterBinding& binding) -> SQLULEN {
        if (param_bind_type_ != 0) {
            return param_bind_type_;
        }
        if (param_bind_offset_ > 0) {
            return static_cast<SQLULEN>(param_bind_offset_);
        }
        auto stride = bytesPerValue(binding);
        if (stride <= 0) {
            return 1;
        }
        return static_cast<SQLULEN>(stride);
    };

    auto rowBaseOffset = [&]() -> SQLULEN {
        if (param_bind_type_ != 0 && param_bind_offset_ > 0) {
            return static_cast<SQLULEN>(param_bind_offset_);
        }
        return 0;
    };

    for (SQLUSMALLINT i = 1; i <= param_bindings_.size(); ++i) {
        auto it = param_bindings_.find(i);
        if (it == param_bindings_.end()) {
            literals.push_back({});
            continue;
        }

        const auto& binding = it->second;

        auto value_base = static_cast<const uint8_t*>(binding.parameter_value);
        auto stride = rowStride(binding);
        size_t offset = static_cast<size_t>(rowBaseOffset()) +
                        static_cast<size_t>(row_offset) * static_cast<size_t>(stride);

        const SQLLEN* ind = indicatorForRow(binding, row_offset);

        if (ind && isDataAtExecIndicator(*ind)) {
            auto key = putDataStreamKey(i, row_offset);
            auto stream_it = put_data_stream_.find(key);
            if (stream_it == put_data_stream_.end() || !stream_it->second.complete) {
                auto init_status = validateOrInitDataAtExecStateForRow(row_offset);
                if (init_status != SQL_SUCCESS) {
                    return init_status;
                }
                stream_it = put_data_stream_.find(key);
                if (stream_it == put_data_stream_.end() || !stream_it->second.complete) {
                    data_at_exec_active_ = true;
                    return SQL_NEED_DATA;
                }
            }

            const auto& stream = stream_it->second;
            ParameterLiteral literal;
            if (stream.truncated) {
                has_info = true;
            }
            if (binding.value_type == SQL_C_BINARY) {
                literal.quoted = false;
                literal.text = "X'" + bytesToHexString(stream.value) + "'";
            } else {
                literal.quoted = true;
                literal.text = stream.value;
            }

            literals.push_back(std::move(literal));
            continue;
        }

        // Check for NULL
        if (ind && *ind == SQL_NULL_DATA) {
            literals.push_back({});
            continue;
        }

        if (!value_base) {
            literals.push_back({});
            continue;
        }

        const void* row_value = value_base + offset;
        ParameterLiteral literal;
        literal.quoted = true;

        switch (binding.value_type) {
            case SQL_C_CHAR: {
                const char* str = static_cast<const char*>(row_value);
                SQLLEN len = (ind && *ind != SQL_NTS) ? *ind : static_cast<SQLLEN>(std::strlen(str));
                if (len < 0) {
                    literals.push_back({});
                    break;
                }
                if (ind && static_cast<size_t>(len) > static_cast<size_t>(binding.buffer_length)) {
                    has_info = true;
                }
                literal.text.assign(str, str + static_cast<size_t>(len));
                break;
            }
            case SQL_C_LONG:
            case SQL_C_SLONG: {
                SQLINTEGER val = *static_cast<const SQLINTEGER*>(row_value);
                literal.text = std::to_string(val);
                literal.quoted = false;
                break;
            }
            case SQL_C_SHORT:
            case SQL_C_SSHORT: {
                SQLSMALLINT val = *static_cast<const SQLSMALLINT*>(row_value);
                literal.text = std::to_string(val);
                literal.quoted = false;
                break;
            }
            case SQL_C_SBIGINT: {
                int64_t val = *static_cast<const int64_t*>(row_value);
                literal.text = std::to_string(val);
                literal.quoted = false;
                break;
            }
            case SQL_C_DOUBLE: {
                SQLDOUBLE val = *static_cast<const SQLDOUBLE*>(row_value);
                literal.text = std::to_string(val);
                literal.quoted = false;
                break;
            }
            case SQL_C_FLOAT: {
                SQLREAL val = *static_cast<const SQLREAL*>(row_value);
                literal.text = std::to_string(val);
                literal.quoted = false;
                break;
            }
            case SQL_C_BIT: {
                unsigned char val = *static_cast<const unsigned char*>(row_value);
                literal.text = val ? "1" : "0";
                literal.quoted = false;
                break;
            }
            case SQL_C_BINARY: {
                SQLLEN len = binding.buffer_length;
                if (ind && *ind >= 0) {
                    len = *ind;
                }
                if (ind && static_cast<size_t>(len) > static_cast<size_t>(binding.buffer_length)) {
                    has_info = true;
                }
                const uint8_t* data = static_cast<const uint8_t*>(row_value);
                std::string hex;
                hex.reserve(static_cast<size_t>(len) * 2);
                static const char kHex[] = "0123456789ABCDEF";
                for (SQLLEN idx = 0; idx < len; ++idx) {
                    uint8_t byte = data[idx];
                    hex.push_back(kHex[(byte >> 4) & 0x0F]);
                    hex.push_back(kHex[byte & 0x0F]);
                }
                literal.text = "X'" + hex + "'";
                literal.quoted = false;
                break;
            }
            case SQL_C_DATE: {
                const auto& date = *static_cast<const SQL_DATE_STRUCT*>(row_value);
                literal.text = formatDateStruct(date);
                break;
            }
            case SQL_C_TIME: {
                const auto& time = *static_cast<const SQL_TIME_STRUCT*>(row_value);
                literal.text = formatTimeStruct(time);
                break;
            }
            case SQL_C_TIMESTAMP: {
                const auto& ts = *static_cast<const SQL_TIMESTAMP_STRUCT*>(row_value);
                literal.text = formatTimestampStruct(ts);
                break;
            }
            case SQL_C_GUID: {
                const auto& guid = *static_cast<const SQLGUID*>(row_value);
                literal.text = formatGuidStruct(guid);
                break;
            }
            default:
                if (binding.buffer_length > 0) {
                    const uint8_t* data = static_cast<const uint8_t*>(row_value);
                    auto len = static_cast<size_t>(binding.buffer_length);
                    literal.text.assign(reinterpret_cast<const char*>(data),
                                       reinterpret_cast<const char*>(data + len));
                }
                break;
        }

        literals.push_back(std::move(literal));
    }

    return has_info ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
}

std::vector<ParameterLiteral> OdbcStatement::buildParameterData() {
    std::vector<ParameterLiteral> result;
    (void)buildParameterData(result, 0);
    return result;
}

void OdbcStatement::setCatalogResult(std::vector<ColumnMetadata> columns,
                                     std::vector<std::vector<std::string>> rows) {
    resetResults();

    ResultSet rs;
    rs.columns = std::move(columns);
    rs.rows = std::move(rows);
    rs.row_count = static_cast<SQLLEN>(rs.rows.size());
    result_sets_.push_back(std::move(rs));

    current_result_index_ = 0;
    applyResultSet(current_result_index_);
    executed_ = true;
    prepared_ = false;
}

SQLRETURN OdbcStatement::executeSqlStatements(const std::string& sql) {
    resetResults();

    if (!conn_) {
        setError("08003", 0, "Connection not open");
        return SQL_ERROR;
    }

    auto statements = splitSqlStatements(sql);
    if (statements.empty()) {
        setError("HY000", 0, "Empty SQL statement");
        return SQL_ERROR;
    }

    SQLRETURN overall_status = SQL_SUCCESS;
    result_sets_.reserve(statements.size());
    for (const auto& statement : statements) {
        ResultSet rs;
        SQLLEN rows_affected = 0;
        auto status = conn_->executeSQL(statement, rs.rows, rs.columns, rows_affected);
        if (status != SQL_SUCCESS && status != SQL_SUCCESS_WITH_INFO) {
            return status;
        }
        if (status == SQL_SUCCESS_WITH_INFO) {
            overall_status = SQL_SUCCESS_WITH_INFO;
        }
        if (!rs.columns.empty()) {
            rs.row_count = static_cast<SQLLEN>(rs.rows.size());
        } else {
            rs.row_count = rows_affected;
        }
        result_sets_.push_back(std::move(rs));
    }

    if (result_sets_.empty()) {
        return SQL_NO_DATA;
    }

    current_result_index_ = 0;
    applyResultSet(current_result_index_);
    executed_ = true;
    return overall_status;
}

void OdbcStatement::applyResultSet(size_t index) {
    if (index >= result_sets_.size()) {
        resetResults();
        return;
    }
    clearGetDataState();

    ResultSet& rs = result_sets_[index];
    columns_ = std::move(rs.columns);
    rows_ = std::move(rs.rows);
    row_count_ = rs.row_count;
    current_row_ = 0;
    has_results_ = !columns_.empty();
}

void OdbcStatement::resetResults() {
    clearGetDataState();
    columns_.clear();
    rows_.clear();
    result_sets_.clear();
    current_row_ = 0;
    row_count_ = -1;
    has_results_ = false;
    current_result_index_ = 0;
}

// Catalog functions
SQLRETURN OdbcStatement::tables(const SQLCHAR* catalog, SQLSMALLINT catalog_len,
                                 const SQLCHAR* schema, SQLSMALLINT schema_len,
                                 const SQLCHAR* table, SQLSMALLINT table_len,
                                 const SQLCHAR* table_type, SQLSMALLINT table_type_len) {
    clearDiagnostics();

    if (!conn_ || !conn_->isConnected()) {
        setError("08003", 0, "Connection not open");
        return SQL_ERROR;
    }

    std::string catalog_pattern = sqlCharToString(catalog, catalog_len);
    std::string schema_pattern = sqlCharToString(schema, schema_len);
    std::string table_pattern = sqlCharToString(table, table_len);
    std::string table_type_pattern = sqlCharToString(table_type, table_type_len);
    bool metadata_id = conn_->getMetadataId();

    std::vector<ColumnMetadata> cols;
    cols.push_back(makeCatalogColumn("TABLE_CAT", SQL_VARCHAR, DriverConfig::MAX_CATALOG_NAME_LEN));
    cols.push_back(makeCatalogColumn("TABLE_SCHEM", SQL_VARCHAR, DriverConfig::MAX_SCHEMA_NAME_LEN));
    cols.push_back(makeCatalogColumn("TABLE_NAME", SQL_VARCHAR, DriverConfig::MAX_TABLE_NAME_LEN));
    cols.push_back(makeCatalogColumn("TABLE_TYPE", SQL_VARCHAR, 32));
    cols.push_back(makeCatalogColumn("REMARKS", SQL_VARCHAR, 255));
    cols.push_back(makeCatalogColumn("TYPE_CAT", SQL_VARCHAR, DriverConfig::MAX_SCHEMA_NAME_LEN));
    cols.push_back(makeCatalogColumn("TYPE_SCHEM", SQL_VARCHAR, DriverConfig::MAX_SCHEMA_NAME_LEN));
    cols.push_back(makeCatalogColumn("TYPE_NAME", SQL_VARCHAR, 128));
    cols.push_back(makeCatalogColumn("SELF_REFERENCING_COL_NAME", SQL_VARCHAR, DriverConfig::MAX_COLUMN_NAME_LEN));
    cols.push_back(makeCatalogColumn("REF_GENERATION", SQL_VARCHAR, 16));

    const std::string& current_catalog = conn_->getCurrentDatabase();

    if (!matchPattern(current_catalog, catalog_pattern, metadata_id)) {
        setCatalogResult(std::move(cols), {});
        return SQL_SUCCESS;
    }

    bool allow_table = true;
    bool allow_view = true;
    bool allow_system_table = true;
    bool allow_system_view = true;
    if (!table_type_pattern.empty()) {
        allow_table = false;
        allow_view = false;
        allow_system_table = false;
        allow_system_view = false;
        std::string upper = toUpper(table_type_pattern);
        std::stringstream ss(upper);
        std::string token;
        while (std::getline(ss, token, ',')) {
            token = trimString(token);
            if (!token.empty() && token.front() == '\'' && token.back() == '\'') {
                token = token.substr(1, token.size() - 2);
            }
            if (token == "TABLE") {
                allow_table = true;
            } else if (token == "VIEW") {
                allow_view = true;
            } else if (token == "SYSTEM VIEW") {
                allow_system_view = true;
            } else if (token == "SYSTEM TABLE") {
                allow_system_table = true;
            } else if (token == "SYSTEM") {
                allow_system_table = true;
                allow_system_view = true;
            }
        }
    }

    if (!allow_table && !allow_view && !allow_system_table && !allow_system_view) {
        setCatalogResult(std::move(cols), {});
        return SQL_SUCCESS;
    }

    std::vector<std::vector<std::string>> table_rows;
    std::vector<ColumnMetadata> table_cols;
    SQLLEN rows_affected = 0;
    auto status = executeCatalogQuery(
        conn_,
        {metadata::kTablesQuery, "SHOW TABLES"},
        table_rows,
        table_cols,
        rows_affected);
    if (status != SQL_SUCCESS) {
        setError("HY000", 0, "Failed to query tables");
        return status;
    }

    std::vector<std::vector<std::string>> rows;
    for (const auto& row : table_rows) {
        if (row.empty()) {
            continue;
        }
        const std::string& table_name = row[0];
        std::string schema_name = (row.size() > 1) ? row[1] : conn_->getCurrentSchema();
        if (schema_name.empty()) {
            schema_name = conn_->getCurrentSchema();
        }
        std::string table_type_value = (row.size() > 2) ? toUpper(trimString(row[2])) : "TABLE";
        if (table_type_value.empty()) {
            table_type_value = "TABLE";
        }
        if (table_type_value == "VIEW" && toUpper(schema_name) == "SYS") {
            table_type_value = "SYSTEM VIEW";
        }

        if (!matchPattern(schema_name, schema_pattern, metadata_id) ||
            !matchPattern(table_name, table_pattern, metadata_id)) {
            continue;
        }

        bool allowed = false;
        if (table_type_value == "TABLE") {
            allowed = allow_table;
        } else if (table_type_value == "VIEW") {
            allowed = allow_view;
        } else if (table_type_value == "SYSTEM TABLE") {
            allowed = allow_system_table;
        } else if (table_type_value == "SYSTEM VIEW") {
            allowed = allow_system_view;
        } else {
            allowed = allow_table;
        }
        if (!allowed) {
            continue;
        }

        rows.push_back({
            current_catalog,
            schema_name,
            table_name,
            table_type_value,
            "",
            "",
            "",
            "",
            "",
            ""
        });
    }

    setCatalogResult(std::move(cols), std::move(rows));
    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::columns(const SQLCHAR* catalog, SQLSMALLINT catalog_len,
                                  const SQLCHAR* schema, SQLSMALLINT schema_len,
                                  const SQLCHAR* table, SQLSMALLINT table_len,
                                  const SQLCHAR* column, SQLSMALLINT column_len) {
    clearDiagnostics();

    if (!conn_ || !conn_->isConnected()) {
        setError("08003", 0, "Connection not open");
        return SQL_ERROR;
    }

    std::string catalog_pattern = sqlCharToString(catalog, catalog_len);
    std::string schema_pattern = sqlCharToString(schema, schema_len);
    std::string table_pattern = sqlCharToString(table, table_len);
    std::string column_pattern = sqlCharToString(column, column_len);
    bool metadata_id = conn_->getMetadataId();

    std::vector<ColumnMetadata> cols;
    cols.push_back(makeCatalogColumn("TABLE_CAT", SQL_VARCHAR, DriverConfig::MAX_CATALOG_NAME_LEN));
    cols.push_back(makeCatalogColumn("TABLE_SCHEM", SQL_VARCHAR, DriverConfig::MAX_SCHEMA_NAME_LEN));
    cols.push_back(makeCatalogColumn("TABLE_NAME", SQL_VARCHAR, DriverConfig::MAX_TABLE_NAME_LEN));
    cols.push_back(makeCatalogColumn("COLUMN_NAME", SQL_VARCHAR, DriverConfig::MAX_COLUMN_NAME_LEN));
    cols.push_back(makeCatalogColumn("DATA_TYPE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("TYPE_NAME", SQL_VARCHAR, 64));
    cols.push_back(makeCatalogColumn("COLUMN_SIZE", SQL_INTEGER));
    cols.push_back(makeCatalogColumn("BUFFER_LENGTH", SQL_INTEGER));
    cols.push_back(makeCatalogColumn("DECIMAL_DIGITS", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("NUM_PREC_RADIX", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("NULLABLE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("REMARKS", SQL_VARCHAR, 255));
    cols.push_back(makeCatalogColumn("COLUMN_DEF", SQL_VARCHAR, 255));
    cols.push_back(makeCatalogColumn("SQL_DATA_TYPE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("SQL_DATETIME_SUB", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("CHAR_OCTET_LENGTH", SQL_INTEGER));
    cols.push_back(makeCatalogColumn("ORDINAL_POSITION", SQL_INTEGER));
    cols.push_back(makeCatalogColumn("IS_NULLABLE", SQL_VARCHAR, 3));

    const std::string& current_catalog = conn_->getCurrentDatabase();

    if (!matchPattern(current_catalog, catalog_pattern, metadata_id)) {
        setCatalogResult(std::move(cols), {});
        return SQL_SUCCESS;
    }

    std::vector<std::vector<std::string>> column_rows;
    std::vector<ColumnMetadata> column_cols;
    SQLLEN rows_affected = 0;
    auto status = conn_->executeSQL(
        "SELECT c.column_name, t.table_name, s.schema_name, c.data_type_name, "
        "c.ordinal_position, c.is_nullable, c.default_value "
        "FROM sys.columns c "
        "JOIN sys.tables t ON t.table_id = c.table_id "
        "JOIN sys.schemas s ON s.schema_id = t.schema_id "
        "WHERE c.is_valid = 1 AND t.is_valid = 1 AND s.is_valid = 1",
        column_rows, column_cols, rows_affected);

    std::vector<std::vector<std::string>> target_tables;
    if (status == SQL_SUCCESS && (!schema_pattern.empty() || !table_pattern.empty() || !column_pattern.empty())) {
        // Metadata path succeeded; keep it if available.
    }

    if (status != SQL_SUCCESS) {
        std::vector<std::vector<std::string>> table_rows;
        std::vector<ColumnMetadata> table_cols;
        auto fallback_status = executeCatalogQuery(
            conn_,
            {metadata::kTablesQuery, "SHOW TABLES"},
            table_rows,
            table_cols,
            rows_affected);
        if (fallback_status != SQL_SUCCESS) {
            setError("HY000", 0, "Failed to query columns");
            return fallback_status;
        }
        for (const auto& table_row : table_rows) {
            if (table_row.empty()) {
                continue;
            }
            const std::string& table_name = table_row[0];
            const std::string schema_name = (table_row.size() > 1) ? table_row[1] : conn_->getCurrentSchema();
            if (!matchPattern(schema_name, schema_pattern, metadata_id) ||
                !matchPattern(table_name, table_pattern, metadata_id)) {
                continue;
            }

            target_tables.push_back({schema_name, table_name});
        }
    } else if (status == SQL_SUCCESS && !column_rows.empty()) {
        for (const auto& col_row : column_rows) {
            if (col_row.size() < 3) {
                continue;
            }
            target_tables.push_back({col_row[2], col_row[1]});
        }
    }

    if (status != SQL_SUCCESS) {
        std::vector<std::vector<std::string>> rows;
        for (const auto& table_row : target_tables) {
            if (table_row.size() < 2) {
                continue;
            }

            const std::string& fallback_schema = table_row[0];
            const std::string& fallback_table = table_row[1];

            std::vector<std::vector<std::string>> show_columns;
            std::vector<ColumnMetadata> show_column_cols;
            auto show_status = conn_->executeSQL("SHOW COLUMNS FROM " + fallback_table,
                                                show_columns, show_column_cols, rows_affected);
            if (show_status != SQL_SUCCESS) {
                setError("HY000", 0, "Failed to query columns");
                return show_status;
            }

            for (size_t index = 0; index < show_columns.size(); ++index) {
                const auto& col_row = show_columns[index];
                if (col_row.size() < 3) {
                    continue;
                }

                const std::string& column_name = col_row[0];
                std::string type_text = col_row[1];
                const std::string& nullable_text = col_row[2];

                if (!matchPattern(column_name, column_pattern, metadata_id)) {
                    continue;
                }

                ParsedTypeInfo type_info = parseTypeString(type_text);
                std::string column_size = type_info.column_size > 0 ? std::to_string(type_info.column_size) : "";
                std::string decimal_digits = type_info.decimal_digits > 0
                                                ? std::to_string(type_info.decimal_digits)
                                                : "";
                std::string radix = type_info.num_prec_radix > 0 ? std::to_string(type_info.num_prec_radix) : "";
                bool nullable = parseBoolValue(nullable_text, true);
                std::string nullable_val = nullable ? std::to_string(SQL_NULLABLE) : std::to_string(SQL_NO_NULLS);
                std::string char_octet = (isCharacterSqlType(type_info.sql_type) ||
                                         isBinarySqlType(type_info.sql_type))
                                            ? column_size
                                            : "";
                std::string default_value;
                if (col_row.size() > 5) {
                    default_value = col_row[5];
                }
                rows.push_back({
                    current_catalog,
                    fallback_schema,
                    fallback_table,
                    column_name,
                    std::to_string(type_info.sql_type),
                    type_info.type_name,
                    column_size,
                    column_size,
                    decimal_digits,
                    radix,
                    nullable_val,
                    "",
                    default_value,
                    std::to_string(type_info.sql_type),
                    "",
                    char_octet,
                    std::to_string(index + 1),
                    nullable ? "YES" : "NO"
                });
            }
        }

        setCatalogResult(std::move(cols), std::move(rows));
        return SQL_SUCCESS;
    }

    std::vector<std::vector<std::string>> rows;
    for (const auto& col_row : column_rows) {
        if (col_row.size() < 7) {
            continue;
        }

        const std::string& column_name = col_row[0];
        const std::string& table_name = col_row[1];
        const std::string& schema_name = col_row[2];
        std::string ordinal_text = col_row[4];
        std::string nullable_text = col_row[5];
        std::string default_value = col_row[6];

        if (!matchPattern(schema_name, schema_pattern, metadata_id) ||
            !matchPattern(table_name, table_pattern, metadata_id) ||
            !matchPattern(column_name, column_pattern, metadata_id)) {
            continue;
        }

        std::string base_type = trimString(col_row[3]);
        if (base_type.empty()) {
            base_type = "UNKNOWN";
        }
        std::string upper_base = toUpper(base_type);

        ParsedTypeInfo type_info = parseTypeString(upper_base);
        type_info.type_name = upper_base;

        std::string column_size = type_info.column_size > 0 ? std::to_string(type_info.column_size) : "";
        std::string decimal_digits = type_info.decimal_digits > 0 ? std::to_string(type_info.decimal_digits) : "";
        std::string radix = type_info.num_prec_radix > 0 ? std::to_string(type_info.num_prec_radix) : "";
        bool nullable = parseBoolValue(nullable_text, true);
        std::string nullable_val = nullable ? std::to_string(SQL_NULLABLE) : std::to_string(SQL_NO_NULLS);
        std::string char_octet = (isCharacterSqlType(type_info.sql_type) || isBinarySqlType(type_info.sql_type))
            ? column_size : "";

        rows.push_back({
            current_catalog,
            schema_name,
            table_name,
            column_name,
            std::to_string(type_info.sql_type),
            type_info.type_name,
            column_size,
            column_size,
            decimal_digits,
            radix,
            nullable_val,
            "",
            default_value,
            std::to_string(type_info.sql_type),
            "",
            char_octet,
            ordinal_text,
            nullable ? "YES" : "NO"
        });
    }

    setCatalogResult(std::move(cols), std::move(rows));
    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::primaryKeys(const SQLCHAR* catalog, SQLSMALLINT catalog_len,
                                      const SQLCHAR* schema, SQLSMALLINT schema_len,
                                      const SQLCHAR* table, SQLSMALLINT table_len) {
    clearDiagnostics();

    if (!conn_ || !conn_->isConnected()) {
        setError("08003", 0, "Connection not open");
        return SQL_ERROR;
    }

    std::string catalog_pattern = sqlCharToString(catalog, catalog_len);
    std::string schema_pattern = sqlCharToString(schema, schema_len);
    std::string table_pattern = sqlCharToString(table, table_len);
    bool metadata_id = conn_->getMetadataId();

    std::vector<ColumnMetadata> cols;
    cols.push_back(makeCatalogColumn("TABLE_CAT", SQL_VARCHAR, DriverConfig::MAX_CATALOG_NAME_LEN));
    cols.push_back(makeCatalogColumn("TABLE_SCHEM", SQL_VARCHAR, DriverConfig::MAX_SCHEMA_NAME_LEN));
    cols.push_back(makeCatalogColumn("TABLE_NAME", SQL_VARCHAR, DriverConfig::MAX_TABLE_NAME_LEN));
    cols.push_back(makeCatalogColumn("COLUMN_NAME", SQL_VARCHAR, DriverConfig::MAX_COLUMN_NAME_LEN));
    cols.push_back(makeCatalogColumn("KEY_SEQ", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("PK_NAME", SQL_VARCHAR, 64));

    const std::string& current_catalog = conn_->getCurrentDatabase();

    if (!matchPattern(current_catalog, catalog_pattern, metadata_id)) {
        setCatalogResult(std::move(cols), {});
        return SQL_SUCCESS;
    }

    std::vector<std::vector<std::string>> pk_rows;
    std::vector<ColumnMetadata> pk_cols;
    SQLLEN rows_affected = 0;
    auto status = conn_->executeSQL(
        "SELECT tc.table_schema, tc.table_name, kcu.column_name, "
        "kcu.ordinal_position, tc.constraint_name "
        "FROM information_schema.table_constraints tc "
        "JOIN information_schema.key_column_usage kcu "
        "  ON tc.constraint_name = kcu.constraint_name "
        " AND tc.table_schema = kcu.table_schema "
        " AND tc.table_name = kcu.table_name "
        "WHERE tc.constraint_type = 'PRIMARY KEY'",
        pk_rows, pk_cols, rows_affected);

    if (status != SQL_SUCCESS) {
        std::vector<std::vector<std::string>> table_rows;
        std::vector<ColumnMetadata> table_cols;
        auto table_status = executeCatalogQuery(
            conn_,
            {metadata::kTablesQuery, "SHOW TABLES"},
            table_rows,
            table_cols,
            rows_affected);
        if (table_status != SQL_SUCCESS) {
            setError("HY000", 0, "Failed to query primary keys");
            return table_status;
        }

        std::vector<std::vector<std::string>> rows;
        for (const auto& table_row : table_rows) {
            if (table_row.empty()) {
                continue;
            }
            const std::string table_name = table_row[0];
            const std::string schema_name = (table_row.size() > 1) ? table_row[1] : conn_->getCurrentSchema();

            if (!matchPattern(schema_name, schema_pattern, metadata_id) ||
                !matchPattern(table_name, table_pattern, metadata_id)) {
                continue;
            }

            std::vector<std::vector<std::string>> show_columns;
            std::vector<ColumnMetadata> show_column_cols;
            auto columns_status = conn_->executeSQL("SHOW COLUMNS FROM " + table_name,
                                                   show_columns, show_column_cols, rows_affected);
            if (columns_status != SQL_SUCCESS) {
                setError("HY000", 0, "Failed to query primary keys");
                return columns_status;
            }

            SQLSMALLINT key_seq = 1;
            for (const auto& col_row : show_columns) {
                if (col_row.size() < 4) {
                    continue;
                }
                if (toUpper(trimString(col_row[3])) != "PRI") {
                    continue;
                }
                rows.push_back({
                    current_catalog,
                    schema_name,
                    table_name,
                    col_row[0],
                    std::to_string(key_seq++),
                    "PRIMARY"
                });
            }
        }

        setCatalogResult(std::move(cols), std::move(rows));
        return SQL_SUCCESS;
    }

    std::vector<std::vector<std::string>> rows;
    for (const auto& row : pk_rows) {
        if (row.size() < 5) {
            continue;
        }
        const std::string& schema_name = row[0];
        const std::string& table_name = row[1];
        const std::string& column_name = row[2];
        const std::string& key_seq = row[3];
        std::string pk_name = row[4];

        if (!matchPattern(schema_name, schema_pattern, metadata_id) ||
            !matchPattern(table_name, table_pattern, metadata_id)) {
            continue;
        }

        if (pk_name.empty()) {
            pk_name = "PRIMARY";
        }
        rows.push_back({
            current_catalog,
            schema_name,
            table_name,
            column_name,
            key_seq,
            pk_name
        });
    }

    setCatalogResult(std::move(cols), std::move(rows));
    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::foreignKeys(const SQLCHAR* pk_catalog, SQLSMALLINT pk_catalog_len,
                                      const SQLCHAR* pk_schema, SQLSMALLINT pk_schema_len,
                                      const SQLCHAR* pk_table, SQLSMALLINT pk_table_len,
                                      const SQLCHAR* fk_catalog, SQLSMALLINT fk_catalog_len,
                                      const SQLCHAR* fk_schema, SQLSMALLINT fk_schema_len,
                                      const SQLCHAR* fk_table, SQLSMALLINT fk_table_len) {
    clearDiagnostics();

    if (!conn_ || !conn_->isConnected()) {
        setError("08003", 0, "Connection not open");
        return SQL_ERROR;
    }

    std::string pk_catalog_pattern = sqlCharToString(pk_catalog, pk_catalog_len);
    std::string pk_schema_pattern = sqlCharToString(pk_schema, pk_schema_len);
    std::string pk_table_pattern = sqlCharToString(pk_table, pk_table_len);
    std::string fk_catalog_pattern = sqlCharToString(fk_catalog, fk_catalog_len);
    std::string fk_schema_pattern = sqlCharToString(fk_schema, fk_schema_len);
    std::string fk_table_pattern = sqlCharToString(fk_table, fk_table_len);
    bool metadata_id = conn_->getMetadataId();

    std::vector<ColumnMetadata> cols;
    cols.push_back(makeCatalogColumn("PKTABLE_CAT", SQL_VARCHAR, DriverConfig::MAX_CATALOG_NAME_LEN));
    cols.push_back(makeCatalogColumn("PKTABLE_SCHEM", SQL_VARCHAR, DriverConfig::MAX_SCHEMA_NAME_LEN));
    cols.push_back(makeCatalogColumn("PKTABLE_NAME", SQL_VARCHAR, DriverConfig::MAX_TABLE_NAME_LEN));
    cols.push_back(makeCatalogColumn("PKCOLUMN_NAME", SQL_VARCHAR, DriverConfig::MAX_COLUMN_NAME_LEN));
    cols.push_back(makeCatalogColumn("FKTABLE_CAT", SQL_VARCHAR, DriverConfig::MAX_CATALOG_NAME_LEN));
    cols.push_back(makeCatalogColumn("FKTABLE_SCHEM", SQL_VARCHAR, DriverConfig::MAX_SCHEMA_NAME_LEN));
    cols.push_back(makeCatalogColumn("FKTABLE_NAME", SQL_VARCHAR, DriverConfig::MAX_TABLE_NAME_LEN));
    cols.push_back(makeCatalogColumn("FKCOLUMN_NAME", SQL_VARCHAR, DriverConfig::MAX_COLUMN_NAME_LEN));
    cols.push_back(makeCatalogColumn("KEY_SEQ", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("UPDATE_RULE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("DELETE_RULE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("FK_NAME", SQL_VARCHAR, 64));
    cols.push_back(makeCatalogColumn("PK_NAME", SQL_VARCHAR, 64));
    cols.push_back(makeCatalogColumn("DEFERRABILITY", SQL_SMALLINT));

    const std::string& current_catalog = conn_->getCurrentDatabase();

    if (!matchPattern(current_catalog, pk_catalog_pattern, metadata_id) ||
        !matchPattern(current_catalog, fk_catalog_pattern, metadata_id)) {
        setCatalogResult(std::move(cols), {});
        return SQL_SUCCESS;
    }

    std::vector<std::vector<std::string>> fk_rows;
    std::vector<ColumnMetadata> fk_cols;
    SQLLEN rows_affected = 0;

    SQLRETURN status = conn_->executeSQL(
        "SELECT tc.table_schema AS fk_schema, tc.table_name AS fk_table, "
        "kcu.column_name AS fk_column, ccu.table_schema AS pk_schema, "
        "ccu.table_name AS pk_table, ccu.column_name AS pk_column, "
        "kcu.ordinal_position AS key_seq, rc.update_rule, rc.delete_rule, "
        "tc.constraint_name AS fk_name, rc.unique_constraint_name AS pk_name, "
        "rc.deferrable AS deferrable "
        "FROM information_schema.table_constraints tc "
        "JOIN information_schema.key_column_usage kcu "
        "  ON tc.constraint_name = kcu.constraint_name "
        " AND tc.table_schema = kcu.table_schema "
        " AND tc.table_name = kcu.table_name "
        "JOIN information_schema.constraint_column_usage ccu "
        "  ON ccu.constraint_name = tc.constraint_name "
        " AND ccu.constraint_schema = tc.table_schema "
        "LEFT JOIN information_schema.referential_constraints rc "
        "  ON rc.constraint_name = tc.constraint_name "
        " AND rc.constraint_schema = tc.table_schema "
        "WHERE tc.constraint_type = 'FOREIGN KEY'",
        fk_rows, fk_cols, rows_affected);
    if (status != SQL_SUCCESS) {
        std::vector<std::vector<std::string>> table_rows;
        std::vector<ColumnMetadata> table_cols;
        auto table_status = conn_->executeSQL(
            "SELECT table_id, schema_id, table_name FROM sb_catalog.sb_tables",
            table_rows, table_cols, rows_affected);
        if (table_status != SQL_SUCCESS) {
            setError("HY000", 0, "Failed to query foreign keys");
            return table_status;
        }

        std::vector<std::vector<std::string>> schema_rows;
        std::vector<ColumnMetadata> schema_cols;
        auto schema_status = conn_->executeSQL(
            "SELECT schema_id, schema_name FROM sb_catalog.sb_schemas",
            schema_rows, schema_cols, rows_affected);
        if (schema_status != SQL_SUCCESS) {
            setError("HY000", 0, "Failed to query foreign keys");
            return schema_status;
        }

        std::map<std::string, std::pair<std::string, std::string>> table_lookup;
        for (const auto& table_row : table_rows) {
            if (table_row.size() < 3) {
                continue;
            }
            const auto table_id = table_row[0];
            const auto schema_id = table_row[1];
            const auto table_name = table_row[2];
            std::string schema_name = schema_id;
            for (const auto& schema_row : schema_rows) {
                if (schema_row.size() >= 2 && schema_row[0] == schema_id) {
                    schema_name = schema_row[1];
                    break;
                }
            }
            table_lookup[table_id] = {schema_name, table_name};
        }

        std::vector<std::vector<std::string>> fk_catalog_rows;
        std::vector<ColumnMetadata> fk_catalog_cols;
        auto fk_status = conn_->executeSQL(
            "SELECT fk_name, child_table_id, parent_table_id, child_columns, parent_columns, "
            "on_update, on_delete, match_type, is_enabled "
            "FROM sb_catalog.sb_foreign_keys",
            fk_catalog_rows, fk_catalog_cols, rows_affected);
        if (fk_status != SQL_SUCCESS) {
            setError("HY000", 0, "Failed to query foreign keys");
            return fk_status;
        }

        std::vector<std::vector<std::string>> rows;
        for (const auto& fk_row : fk_catalog_rows) {
            if (fk_row.size() < 9) {
                continue;
            }
            const std::string fk_name = fk_row[0];
            const auto child_table_id = fk_row[1];
            const auto parent_table_id = fk_row[2];
            const auto child_columns = splitCsvColumns(fk_row[3]);
            const auto parent_columns = splitCsvColumns(fk_row[4]);
            const auto& on_update = fk_row[5];
            const auto& on_delete = fk_row[6];
            const auto& deferrable = fk_row[8];

            auto child_it = table_lookup.find(child_table_id);
            auto parent_it = table_lookup.find(parent_table_id);
            if (child_it == table_lookup.end() || parent_it == table_lookup.end()) {
                continue;
            }

            const auto& child_schema = child_it->second.first;
            const auto& child_table = child_it->second.second;
            const auto& parent_schema = parent_it->second.first;
            const auto& parent_table = parent_it->second.second;

            if (!matchPattern(parent_schema, pk_schema_pattern, metadata_id) ||
                !matchPattern(child_schema, fk_schema_pattern, metadata_id) ||
                !matchPattern(parent_table, pk_table_pattern, metadata_id) ||
                !matchPattern(child_table, fk_table_pattern, metadata_id)) {
                continue;
            }

            const auto pair_count = static_cast<int>(std::min(child_columns.size(), parent_columns.size()));
            for (int idx = 0; idx < pair_count; ++idx) {
                SQLSMALLINT update_rule = mapFkRuleToOdbc(on_update);
                SQLSMALLINT delete_rule = mapFkRuleToOdbc(on_delete);
                SQLSMALLINT deferrability = mapDeferrabilityToOdbc(deferrable);

                rows.push_back({
                    current_catalog,
                    parent_schema,
                    parent_table,
                    parent_columns[idx],
                    current_catalog,
                    child_schema,
                    child_table,
                    child_columns[idx],
                    std::to_string(idx + 1),
                    std::to_string(update_rule),
                    std::to_string(delete_rule),
                    fk_name,
                    "PRIMARY",
                    std::to_string(deferrability)
                });
            }
        }

        setCatalogResult(std::move(cols), std::move(rows));
        return SQL_SUCCESS;
    }

    std::vector<std::vector<std::string>> rows;
    for (const auto& fk_row : fk_rows) {
        if (fk_row.size() < 12) {
            continue;
        }

        const std::string& fk_schema_name = fk_row[0];
        const std::string& fk_table_name = fk_row[1];
        const std::string& fk_column_name = fk_row[2];
        const std::string& pk_schema_name = fk_row[3];
        const std::string& pk_table_name = fk_row[4];
        const std::string& pk_column_name = fk_row[5];
        const std::string& key_seq = fk_row[6];
        const std::string& on_update = fk_row[7];
        const std::string& on_delete = fk_row[8];
        const std::string& fk_name = fk_row[9];
        const std::string& pk_name = fk_row[10];
        const std::string& deferrable = fk_row[11];

        if (!matchPattern(pk_schema_name, pk_schema_pattern, metadata_id) ||
            !matchPattern(fk_schema_name, fk_schema_pattern, metadata_id)) {
            continue;
        }
        if (!matchPattern(pk_table_name, pk_table_pattern, metadata_id) ||
            !matchPattern(fk_table_name, fk_table_pattern, metadata_id)) {
            continue;
        }

        SQLSMALLINT update_rule = mapFkRuleToOdbc(on_update);
        SQLSMALLINT delete_rule = mapFkRuleToOdbc(on_delete);
        SQLSMALLINT deferrability = mapDeferrabilityToOdbc(deferrable);

        rows.push_back({
            current_catalog,
            pk_schema_name,
            pk_table_name,
            pk_column_name,
            current_catalog,
            fk_schema_name,
            fk_table_name,
            fk_column_name,
            key_seq,
            std::to_string(update_rule),
            std::to_string(delete_rule),
            fk_name,
            pk_name,
            std::to_string(deferrability)
        });
    }

    setCatalogResult(std::move(cols), std::move(rows));
    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::statistics(const SQLCHAR* catalog, SQLSMALLINT catalog_len,
                                     const SQLCHAR* schema, SQLSMALLINT schema_len,
                                     const SQLCHAR* table, SQLSMALLINT table_len,
                                     SQLUSMALLINT unique, SQLUSMALLINT reserved) {
    clearDiagnostics();

    if (!conn_ || !conn_->isConnected()) {
        setError("08003", 0, "Connection not open");
        return SQL_ERROR;
    }

    std::string catalog_pattern = sqlCharToString(catalog, catalog_len);
    std::string schema_pattern = sqlCharToString(schema, schema_len);
    std::string table_pattern = sqlCharToString(table, table_len);
    bool metadata_id = conn_->getMetadataId();

    std::vector<ColumnMetadata> cols;
    cols.push_back(makeCatalogColumn("TABLE_CAT", SQL_VARCHAR, DriverConfig::MAX_CATALOG_NAME_LEN));
    cols.push_back(makeCatalogColumn("TABLE_SCHEM", SQL_VARCHAR, DriverConfig::MAX_SCHEMA_NAME_LEN));
    cols.push_back(makeCatalogColumn("TABLE_NAME", SQL_VARCHAR, DriverConfig::MAX_TABLE_NAME_LEN));
    cols.push_back(makeCatalogColumn("NON_UNIQUE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("INDEX_QUALIFIER", SQL_VARCHAR, DriverConfig::MAX_SCHEMA_NAME_LEN));
    cols.push_back(makeCatalogColumn("INDEX_NAME", SQL_VARCHAR, 64));
    cols.push_back(makeCatalogColumn("TYPE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("ORDINAL_POSITION", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("COLUMN_NAME", SQL_VARCHAR, DriverConfig::MAX_COLUMN_NAME_LEN));
    cols.push_back(makeCatalogColumn("ASC_OR_DESC", SQL_CHAR, 1));
    cols.push_back(makeCatalogColumn("CARDINALITY", SQL_INTEGER));
    cols.push_back(makeCatalogColumn("PAGES", SQL_INTEGER));
    cols.push_back(makeCatalogColumn("FILTER_CONDITION", SQL_VARCHAR, 255));

    const std::string& current_catalog = conn_->getCurrentDatabase();

    if (!matchPattern(current_catalog, catalog_pattern, metadata_id)) {
        setCatalogResult(std::move(cols), {});
        return SQL_SUCCESS;
    }

    std::vector<std::vector<std::string>> index_rows;
    std::vector<ColumnMetadata> index_cols;
    SQLLEN rows_affected = 0;
    auto status = conn_->executeSQL(
        "SELECT s.schema_name, t.table_name, i.index_name, i.is_unique, "
        "ic.ordinal_position, ic.column_name, ic.is_included "
        "FROM sys.indexes i "
        "JOIN sys.index_columns ic ON ic.index_id = i.index_id "
        "JOIN sys.tables t ON t.table_id = i.table_id "
        "JOIN sys.schemas s ON s.schema_id = t.schema_id "
        "WHERE i.is_valid = 1 AND t.is_valid = 1 AND s.is_valid = 1",
        index_rows, index_cols, rows_affected);

    bool require_unique = (unique == SQL_INDEX_UNIQUE);
    std::unordered_map<std::string, SQLSMALLINT> ordinal_map;

    std::vector<std::vector<std::string>> rows;
    if (status == SQL_SUCCESS) {
        for (const auto& idx_row : index_rows) {
            if (idx_row.size() < 7) {
                continue;
            }

            const std::string& schema_name = idx_row[0];
            const std::string& table_name = idx_row[1];
            const std::string& index_name = idx_row[2];
            bool is_unique = parseBoolValue(idx_row[3]);
            std::string ordinal_text = idx_row[4];
            std::string column_name = idx_row[5];
            bool is_included = parseBoolValue(idx_row[6]);

            if (!matchPattern(schema_name, schema_pattern, metadata_id) ||
                !matchPattern(table_name, table_pattern, metadata_id)) {
                continue;
            }

            if (require_unique && !is_unique) {
                continue;
            }
            if (is_included) {
                continue;
            }

            SQLSMALLINT ordinal = 0;
            if (!ordinal_text.empty()) {
                int64_t parsed = 0;
                if (parseInt64(ordinal_text, parsed) && parsed > 0) {
                    ordinal = static_cast<SQLSMALLINT>(parsed);
                }
            }
            if (ordinal == 0) {
                ordinal = ++ordinal_map[index_name];
            }

            (void)reserved;

            rows.push_back({
                current_catalog,
                schema_name,
                table_name,
                is_unique ? "0" : "1",
                current_catalog,
                index_name,
                std::to_string(SQL_INDEX_OTHER),
                std::to_string(ordinal),
                column_name,
                "",
                "",
                "",
                ""
            });
        }
    } else {
        std::vector<std::vector<std::string>> table_rows;
        std::vector<ColumnMetadata> table_cols;
        auto table_status = executeCatalogQuery(
            conn_,
            {metadata::kTablesQuery, "SHOW TABLES"},
            table_rows,
            table_cols,
            rows_affected);
        if (table_status != SQL_SUCCESS) {
            setError("HY000", 0, "Failed to query indexes");
            return table_status;
        }

        for (const auto& table_row : table_rows) {
            if (table_row.empty()) {
                continue;
            }
            const std::string table_name = table_row[0];
            const std::string schema_name = (table_row.size() > 1) ? table_row[1] : conn_->getCurrentSchema();

            if (!matchPattern(schema_name, schema_pattern, metadata_id) ||
                !matchPattern(table_name, table_pattern, metadata_id)) {
                continue;
            }

            std::vector<std::vector<std::string>> index_rows_show;
            std::vector<ColumnMetadata> index_cols_show;
            auto index_status = conn_->executeSQL(
                "SHOW INDEXES FROM " + table_name,
                index_rows_show, index_cols_show, rows_affected);
            if (index_status != SQL_SUCCESS) {
                setError("HY000", 0, "Failed to query indexes");
                return index_status;
            }

            for (const auto& idx_row : index_rows_show) {
                if (idx_row.size() < 5) {
                    continue;
                }
                const std::string& index_schema = schema_name;
                const std::string& index_name = idx_row[2];
                std::string non_unique_text = idx_row[1];
                bool is_unique = (non_unique_text == "0" || toUpper(non_unique_text) == "NO");
                if (require_unique && !is_unique) {
                    continue;
                }
                SQLSMALLINT ordinal = ++ordinal_map[index_name];

                (void)reserved;
                rows.push_back({
                    current_catalog,
                    index_schema,
                    table_name,
                    is_unique ? "0" : "1",
                    current_catalog,
                    index_name,
                    std::to_string(SQL_INDEX_OTHER),
                    std::to_string(ordinal),
                    idx_row[3],
                    "",
                    "",
                    "",
                    ""
                });
            }
        }
    }

    setCatalogResult(std::move(cols), std::move(rows));
    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::specialColumns(SQLUSMALLINT /*identifier_type*/,
                                         const SQLCHAR* catalog, SQLSMALLINT catalog_len,
                                         const SQLCHAR* schema, SQLSMALLINT schema_len,
                                         const SQLCHAR* table, SQLSMALLINT table_len,
                                         SQLUSMALLINT /*scope*/, SQLUSMALLINT /*nullable*/) {
    clearDiagnostics();

    if (!conn_ || !conn_->isConnected()) {
        setError("08003", 0, "Connection not open");
        return SQL_ERROR;
    }

    std::string catalog_pattern = sqlCharToString(catalog, catalog_len);
    std::string schema_pattern = sqlCharToString(schema, schema_len);
    std::string table_pattern = sqlCharToString(table, table_len);
    bool metadata_id = conn_->getMetadataId();

    std::vector<ColumnMetadata> cols;
    cols.push_back(makeCatalogColumn("SCOPE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("COLUMN_NAME", SQL_VARCHAR, DriverConfig::MAX_COLUMN_NAME_LEN));
    cols.push_back(makeCatalogColumn("DATA_TYPE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("TYPE_NAME", SQL_VARCHAR, 64));
    cols.push_back(makeCatalogColumn("COLUMN_SIZE", SQL_INTEGER));
    cols.push_back(makeCatalogColumn("BUFFER_LENGTH", SQL_INTEGER));
    cols.push_back(makeCatalogColumn("DECIMAL_DIGITS", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("PSEUDO_COLUMN", SQL_SMALLINT));

    const std::string& current_catalog = conn_->getCurrentDatabase();
    const std::string& current_schema = conn_->getCurrentSchema();

    if (!matchPattern(current_catalog, catalog_pattern, metadata_id) ||
        !matchPattern(current_schema, schema_pattern, metadata_id)) {
        setCatalogResult(std::move(cols), {});
        return SQL_SUCCESS;
    }

    std::vector<std::vector<std::string>> show_tables;
    std::vector<ColumnMetadata> show_table_cols;
    SQLLEN rows_affected = 0;
    auto status = conn_->executeSQL("SHOW TABLES", show_tables, show_table_cols, rows_affected);
    if (status != SQL_SUCCESS) {
        setError("HY000", 0, "Failed to query tables");
        return status;
    }

    std::vector<std::vector<std::string>> rows;
    for (const auto& table_row : show_tables) {
        if (table_row.empty()) {
            continue;
        }
        const std::string& table_name = table_row[0];
        if (!matchPattern(table_name, table_pattern, metadata_id)) {
            continue;
        }

        std::vector<std::vector<std::string>> show_columns;
        std::vector<ColumnMetadata> show_column_cols;
        status = conn_->executeSQL("SHOW COLUMNS FROM " + table_name, show_columns,
                                   show_column_cols, rows_affected);
        if (status != SQL_SUCCESS) {
            setError("HY000", 0, "Failed to query columns");
            return status;
        }

        for (const auto& col_row : show_columns) {
            if (col_row.size() < 4) {
                continue;
            }
            std::string key_flag = toUpper(trimString(col_row[3]));
            if (key_flag != "PRI") {
                continue;
            }
            ParsedTypeInfo type_info = parseTypeString(col_row[1]);
            std::string column_size = type_info.column_size > 0 ? std::to_string(type_info.column_size) : "";
            std::string decimal_digits = type_info.decimal_digits > 0 ? std::to_string(type_info.decimal_digits) : "";

            rows.push_back({
                "2",
                col_row[0],
                std::to_string(type_info.sql_type),
                type_info.type_name,
                column_size,
                column_size,
                decimal_digits,
                "1"
            });
        }
    }

    setCatalogResult(std::move(cols), std::move(rows));
    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::procedures(const SQLCHAR* catalog, SQLSMALLINT catalog_len,
                                     const SQLCHAR* schema, SQLSMALLINT schema_len,
                                     const SQLCHAR* proc, SQLSMALLINT proc_len) {
    clearDiagnostics();

    if (!conn_ || !conn_->isConnected()) {
        setError("08003", 0, "Connection not open");
        return SQL_ERROR;
    }

    std::string catalog_pattern = sqlCharToString(catalog, catalog_len);
    std::string schema_pattern = sqlCharToString(schema, schema_len);
    std::string proc_pattern = sqlCharToString(proc, proc_len);
    bool metadata_id = conn_->getMetadataId();

    std::vector<ColumnMetadata> cols;
    cols.push_back(makeCatalogColumn("PROCEDURE_CAT", SQL_VARCHAR, DriverConfig::MAX_CATALOG_NAME_LEN));
    cols.push_back(makeCatalogColumn("PROCEDURE_SCHEM", SQL_VARCHAR, DriverConfig::MAX_SCHEMA_NAME_LEN));
    cols.push_back(makeCatalogColumn("PROCEDURE_NAME", SQL_VARCHAR, 128));
    cols.push_back(makeCatalogColumn("NUM_INPUT_PARAMS", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("NUM_OUTPUT_PARAMS", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("NUM_RESULT_SETS", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("REMARKS", SQL_VARCHAR, 255));
    cols.push_back(makeCatalogColumn("PROCEDURE_TYPE", SQL_SMALLINT));

    const std::string& current_catalog = conn_->getCurrentDatabase();
    if (!matchPattern(current_catalog, catalog_pattern, metadata_id)) {
        setCatalogResult(std::move(cols), {});
        return SQL_SUCCESS;
    }

    struct ProcedureCounts {
        int64_t input_count = 0;
        int64_t output_count = 0;
    };

    using ProcedureKey = std::pair<std::string, std::string>;
    std::map<ProcedureKey, ProcedureCounts> proc_counts;

    std::vector<std::vector<std::string>> routine_rows;
    std::vector<ColumnMetadata> routine_cols;
    SQLLEN rows_affected = 0;
    auto status = conn_->executeSQL(
        "SELECT routine_schema, routine_name, routine_type, data_type "
        "FROM information_schema.routines "
        "ORDER BY routine_schema, routine_name",
        routine_rows, routine_cols, rows_affected);
    if (status != SQL_SUCCESS) {
        setError("HY000", 0, "Failed to query routines");
        return status;
    }

    std::vector<std::vector<std::string>> parameter_rows;
    std::vector<ColumnMetadata> parameter_cols;
    status = conn_->executeSQL(
        "SELECT routine_schema, routine_name, parameter_mode, parameter_name, data_type "
        "FROM information_schema.parameters "
        "ORDER BY routine_schema, routine_name, ordinal_position",
        parameter_rows, parameter_cols, rows_affected);
    if (status != SQL_SUCCESS) {
        setError("HY000", 0, "Failed to query parameters");
        return status;
    }

    for (const auto& param_row : parameter_rows) {
        if (param_row.size() < 5) {
            continue;
        }

        const std::string& routine_schema = param_row[0];
        const std::string& routine_name = param_row[1];
        const std::string& mode_text = param_row[2];

        if (!matchPattern(routine_schema, schema_pattern, metadata_id) ||
            !matchPattern(routine_name, proc_pattern, metadata_id)) {
            continue;
        }

        ProcedureKey key(routine_schema, routine_name);
        auto& counters = proc_counts[key];
        SQLSMALLINT mode = parseProcedureColumnMode(mode_text);
        if (mode == SQL_PARAM_INPUT || mode == SQL_PARAM_INPUT_OUTPUT) {
            ++counters.input_count;
        }
        if (mode == SQL_PARAM_OUTPUT || mode == SQL_PARAM_INPUT_OUTPUT) {
            ++counters.output_count;
        }
    }

    std::vector<std::vector<std::string>> rows;
    for (const auto& routine_row : routine_rows) {
        if (routine_row.size() < 4) {
            continue;
        }

        const std::string& routine_schema = routine_row[0];
        const std::string& routine_name = routine_row[1];
        const std::string& routine_type = routine_row[2];

        if (!matchPattern(routine_schema, schema_pattern, metadata_id) ||
            !matchPattern(routine_name, proc_pattern, metadata_id)) {
            continue;
        }

        SQLSMALLINT proc_type = parseProcedureType(routine_type);
        SQLSMALLINT result_set_count = (proc_type == SQL_PT_FUNCTION) ? 1 : 0;

        auto it = proc_counts.find(ProcedureKey(routine_schema, routine_name));
        int64_t num_input = it != proc_counts.end() ? it->second.input_count : 0;
        int64_t num_output = it != proc_counts.end() ? it->second.output_count : 0;

        rows.push_back({
            current_catalog,
            routine_schema,
            routine_name,
            std::to_string(num_input),
            std::to_string(num_output),
            std::to_string(result_set_count),
            "",
            std::to_string(proc_type)
        });
    }

    setCatalogResult(std::move(cols), std::move(rows));
    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::procedureColumns(const SQLCHAR* catalog, SQLSMALLINT catalog_len,
                                           const SQLCHAR* schema, SQLSMALLINT schema_len,
                                           const SQLCHAR* proc, SQLSMALLINT proc_len,
                                           const SQLCHAR* column, SQLSMALLINT column_len) {
    clearDiagnostics();

    if (!conn_ || !conn_->isConnected()) {
        setError("08003", 0, "Connection not open");
        return SQL_ERROR;
    }

    std::string catalog_pattern = sqlCharToString(catalog, catalog_len);
    std::string schema_pattern = sqlCharToString(schema, schema_len);
    std::string proc_pattern = sqlCharToString(proc, proc_len);
    std::string column_pattern = sqlCharToString(column, column_len);
    bool metadata_id = conn_->getMetadataId();

    std::vector<ColumnMetadata> cols;
    cols.push_back(makeCatalogColumn("PROCEDURE_CAT", SQL_VARCHAR, DriverConfig::MAX_CATALOG_NAME_LEN));
    cols.push_back(makeCatalogColumn("PROCEDURE_SCHEM", SQL_VARCHAR, DriverConfig::MAX_SCHEMA_NAME_LEN));
    cols.push_back(makeCatalogColumn("PROCEDURE_NAME", SQL_VARCHAR, 128));
    cols.push_back(makeCatalogColumn("COLUMN_NAME", SQL_VARCHAR, DriverConfig::MAX_COLUMN_NAME_LEN));
    cols.push_back(makeCatalogColumn("COLUMN_TYPE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("DATA_TYPE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("TYPE_NAME", SQL_VARCHAR, 64));
    cols.push_back(makeCatalogColumn("COLUMN_SIZE", SQL_INTEGER));
    cols.push_back(makeCatalogColumn("BUFFER_LENGTH", SQL_INTEGER));
    cols.push_back(makeCatalogColumn("DECIMAL_DIGITS", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("NUM_PREC_RADIX", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("NULLABLE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("REMARKS", SQL_VARCHAR, 255));
    cols.push_back(makeCatalogColumn("COLUMN_DEF", SQL_VARCHAR, 255));
    cols.push_back(makeCatalogColumn("SQL_DATA_TYPE", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("SQL_DATETIME_SUB", SQL_SMALLINT));
    cols.push_back(makeCatalogColumn("CHAR_OCTET_LENGTH", SQL_INTEGER));
    cols.push_back(makeCatalogColumn("ORDINAL_POSITION", SQL_INTEGER));
    cols.push_back(makeCatalogColumn("IS_NULLABLE", SQL_VARCHAR, 3));

    const std::string& current_catalog = conn_->getCurrentDatabase();
    if (!matchPattern(current_catalog, catalog_pattern, metadata_id)) {
        setCatalogResult(std::move(cols), {});
        return SQL_SUCCESS;
    }

    using ProcKey = std::pair<std::string, std::string>;
    std::map<ProcKey, std::string> function_return_types;

    std::vector<std::vector<std::string>> routine_rows;
    std::vector<ColumnMetadata> routine_cols;
    SQLLEN rows_affected = 0;
    auto status = conn_->executeSQL(
        "SELECT routine_schema, routine_name, routine_type, data_type "
        "FROM information_schema.routines "
        "ORDER BY routine_schema, routine_name",
        routine_rows, routine_cols, rows_affected);
    if (status != SQL_SUCCESS) {
        setError("HY000", 0, "Failed to query routine types");
        return status;
    }

    for (const auto& routine_row : routine_rows) {
        if (routine_row.size() < 4) {
            continue;
        }

        const std::string& routine_schema = routine_row[0];
        const std::string& routine_name = routine_row[1];
        if (!matchPattern(routine_schema, schema_pattern, metadata_id) ||
            !matchPattern(routine_name, proc_pattern, metadata_id)) {
            continue;
        }
        if (parseProcedureType(routine_row[2]) == SQL_PT_FUNCTION) {
            function_return_types.emplace(ProcKey(routine_schema, routine_name), routine_row[3]);
        }
    }

    std::vector<std::vector<std::string>> parameter_rows;
    std::vector<ColumnMetadata> parameter_cols;
    status = conn_->executeSQL(
        "SELECT routine_schema, routine_name, ordinal_position, parameter_mode, "
        "parameter_name, data_type, character_maximum_length, numeric_precision, numeric_scale "
        "FROM information_schema.parameters "
        "ORDER BY routine_schema, routine_name, ordinal_position",
        parameter_rows, parameter_cols, rows_affected);
    if (status != SQL_SUCCESS) {
        setError("HY000", 0, "Failed to query procedure parameters");
        return status;
    }

    std::vector<std::vector<std::string>> rows;
    std::map<ProcKey, bool> return_row_emitted;
    for (const auto& param_row : parameter_rows) {
        if (param_row.size() < 9) {
            continue;
        }

        const std::string& routine_schema = param_row[0];
        const std::string& routine_name = param_row[1];
        const std::string& ordinal_position = param_row[2];
        const std::string& parameter_mode = param_row[3];
        const std::string& parameter_name = param_row[4];
        const std::string& data_type = param_row[5];
        const std::string& character_max_length = param_row[6];
        const std::string& numeric_precision = param_row[7];
        const std::string& numeric_scale = param_row[8];

        if (!matchPattern(routine_schema, schema_pattern, metadata_id) ||
            !matchPattern(routine_name, proc_pattern, metadata_id) ||
            !matchPattern(parameter_name, column_pattern, metadata_id)) {
            continue;
        }

        ProcKey proc_key(routine_schema, routine_name);
        ParsedTypeInfo type_info = parseTypeString(trimString(data_type));
        if (type_info.column_size == 0) {
            int64_t tmp = 0;
            if (parseInt64(character_max_length, tmp) && tmp > 0) {
                type_info.column_size = static_cast<SQLULEN>(tmp);
            } else if (parseInt64(numeric_precision, tmp) && tmp > 0) {
                type_info.column_size = static_cast<SQLULEN>(tmp);
            }
        }

        int64_t parsed_scale = 0;
        if (parseInt64(numeric_scale, parsed_scale)) {
            type_info.decimal_digits = static_cast<SQLSMALLINT>(parsed_scale);
        }

        std::string column_size = type_info.column_size > 0 ? std::to_string(type_info.column_size) : "";
        std::string decimal_digits = type_info.decimal_digits > 0 ? std::to_string(type_info.decimal_digits) : "";
        std::string radix = type_info.num_prec_radix > 0 ? std::to_string(type_info.num_prec_radix) : "";
        std::string char_octet = (isCharacterSqlType(type_info.sql_type) || isBinarySqlType(type_info.sql_type))
                                ? column_size : "";
        SQLSMALLINT column_type = parseProcedureColumnMode(parameter_mode);

        if (column_type == kOdbcProcedureColumnReturn) {
            return_row_emitted[proc_key] = true;
        }

        rows.push_back({
            current_catalog,
            routine_schema,
            routine_name,
            parameter_name,
            std::to_string(column_type),
            std::to_string(type_info.sql_type),
            type_info.type_name,
            column_size,
            column_size,
            decimal_digits,
            radix,
            std::to_string(SQL_NULLABLE),
            "",
            "",
            std::to_string(type_info.sql_type),
            "",
            char_octet,
            ordinal_position,
            "YES"
        });
    }

    for (const auto& function_item : function_return_types) {
        const ProcKey& proc_key = function_item.first;
        const std::string& routine_schema = proc_key.first;
        const std::string& routine_name = proc_key.second;

        if (!matchPattern(routine_schema, schema_pattern, metadata_id) ||
            !matchPattern(routine_name, proc_pattern, metadata_id) ||
            !matchPattern("RETURN_VALUE", column_pattern, metadata_id)) {
            continue;
        }

        if (return_row_emitted[proc_key]) {
            continue;
        }

        ParsedTypeInfo type_info = parseTypeString(trimString(function_item.second));
        std::string column_size = type_info.column_size > 0 ? std::to_string(type_info.column_size) : "";
        std::string radix = type_info.num_prec_radix > 0 ? std::to_string(type_info.num_prec_radix) : "";
        std::string char_octet = (isCharacterSqlType(type_info.sql_type) || isBinarySqlType(type_info.sql_type))
                                ? column_size : "";

        rows.push_back({
            current_catalog,
            routine_schema,
            routine_name,
            "RETURN_VALUE",
            std::to_string(kOdbcProcedureColumnReturn),
            std::to_string(type_info.sql_type),
            type_info.type_name,
            column_size,
            column_size,
            "",
            radix,
            std::to_string(SQL_NULLABLE),
            "",
            "",
            std::to_string(type_info.sql_type),
            "",
            char_octet,
            "0",
            "YES"
        });
    }

    setCatalogResult(std::move(cols), std::move(rows));
    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::tablePrivileges(const SQLCHAR* catalog, SQLSMALLINT catalog_len,
                                          const SQLCHAR* schema, SQLSMALLINT schema_len,
                                          const SQLCHAR* table, SQLSMALLINT table_len) {
    clearDiagnostics();

    if (!conn_ || !conn_->isConnected()) {
        setError("08003", 0, "Connection not open");
        return SQL_ERROR;
    }

    std::string catalog_pattern = sqlCharToString(catalog, catalog_len);
    std::string schema_pattern = sqlCharToString(schema, schema_len);
    std::string table_pattern = sqlCharToString(table, table_len);
    bool metadata_id = conn_->getMetadataId();
    const bool has_explicit_schema = !schema_pattern.empty();
    bool inferred_schema_from_table = false;

    if (schema_pattern.empty() && !table_pattern.empty()) {
        const auto parsed_table = parsePrivilegeObjectPath(table_pattern);
        if (!parsed_table.schema.empty()) {
            schema_pattern = parsed_table.schema;
            if (!parsed_table.table.empty()) {
                table_pattern = parsed_table.table;
            }
            inferred_schema_from_table = true;
        }
    }

    std::vector<ColumnMetadata> cols;
    cols.push_back(makeCatalogColumn("TABLE_CAT", SQL_VARCHAR, DriverConfig::MAX_CATALOG_NAME_LEN));
    cols.push_back(makeCatalogColumn("TABLE_SCHEM", SQL_VARCHAR, DriverConfig::MAX_SCHEMA_NAME_LEN));
    cols.push_back(makeCatalogColumn("TABLE_NAME", SQL_VARCHAR, DriverConfig::MAX_TABLE_NAME_LEN));
    cols.push_back(makeCatalogColumn("GRANTOR", SQL_VARCHAR, 64));
    cols.push_back(makeCatalogColumn("GRANTEE", SQL_VARCHAR, 64));
    cols.push_back(makeCatalogColumn("PRIVILEGE", SQL_VARCHAR, 64));
    cols.push_back(makeCatalogColumn("IS_GRANTABLE", SQL_VARCHAR, 3));

    const std::string& current_catalog = conn_->getCurrentDatabase();

    if (!matchPattern(current_catalog, catalog_pattern, metadata_id)) {
        setCatalogResult(std::move(cols), {});
        return SQL_SUCCESS;
    }

    std::vector<std::vector<std::string>> grant_rows;
    auto grant_status = queryShowGrants(conn_, grant_rows);
    if (grant_status != SQL_SUCCESS) {
        setError("HY000", 0, "Failed to execute SHOW GRANTS");
        return grant_status;
    }

    std::vector<std::vector<std::string>> rows;
    for (const auto& grant_row : grant_rows) {
        if (std::getenv("ODBC_DEBUG_PRIV")) {
            std::cerr << "DEBUG PRIV ROW size=" << grant_row.size();
            if (grant_row.size() > 2) {
                std::cerr << " object=" << grant_row[1]
                          << " privilege=" << grant_row[2]
                          << " grantor=" << grant_row[0]
                          << " grantable=" << grant_row[4] << "\n";
            } else {
                std::cerr << "\n";
            }
        }
        if (grant_row.size() < 5) {
            if (std::getenv("ODBC_DEBUG_PRIV")) {
                std::cerr << "DEBUG PRIV SKIP SIZE\n";
            }
            continue;
        }
        if (isRoleGrantObject(grant_row[1], grant_row[2])) {
            if (std::getenv("ODBC_DEBUG_PRIV")) {
                std::cerr << "DEBUG PRIV SKIP ROLE\n";
            }
            continue;
        }

        const auto path = parsePrivilegeObjectPath(grant_row[1]);
        const bool include_column_privileges = inferred_schema_from_table && has_explicit_schema == false;
        if (path.table.empty() || (path.has_column && !include_column_privileges)) {
            if (std::getenv("ODBC_DEBUG_PRIV")) {
                std::cerr << "DEBUG PRIV PARSE path schema=" << path.schema
                          << " table=" << path.table
                          << " has_column=" << path.has_column
                          << " -> SKIP\n";
            }
            continue;
        }
        if (!matchSchemaPattern(path, schema_pattern, metadata_id)) {
            if (std::getenv("ODBC_DEBUG_PRIV")) {
                std::cerr << "DEBUG PRIV SKIP SCHEMA pattern mismatch path.schema=" << path.schema
                          << " pattern=" << schema_pattern << "\n";
            }
            continue;
        }
        if (!matchTablePattern(path, table_pattern, metadata_id)) {
            if (std::getenv("ODBC_DEBUG_PRIV")) {
                std::cerr << "DEBUG PRIV SKIP TABLE pattern mismatch path.table=" << path.table
                          << " pattern=" << table_pattern << "\n";
            }
            continue;
        }

        rows.push_back({
            current_catalog,
            path.schema,
            path.table,
            grant_row[3],
            grant_row[0],
            grant_row[2],
            normalizeGrantOption(grant_row[4])
        });
        if (std::getenv("ODBC_DEBUG_PRIV")) {
            std::cerr << "DEBUG PRIV PUSH schema=" << path.schema << " table=" << path.table
                      << " priv=" << grant_row[2] << "\n";
        }
    }

    setCatalogResult(std::move(cols), std::move(rows));
    return SQL_SUCCESS;
}

SQLRETURN OdbcStatement::columnPrivileges(const SQLCHAR* catalog, SQLSMALLINT catalog_len,
                                           const SQLCHAR* schema, SQLSMALLINT schema_len,
                                           const SQLCHAR* table, SQLSMALLINT table_len,
                                           const SQLCHAR* column, SQLSMALLINT column_len) {
    clearDiagnostics();

    if (!conn_ || !conn_->isConnected()) {
        setError("08003", 0, "Connection not open");
        return SQL_ERROR;
    }

    std::string catalog_pattern = sqlCharToString(catalog, catalog_len);
    std::string schema_pattern = sqlCharToString(schema, schema_len);
    std::string table_pattern = sqlCharToString(table, table_len);
    std::string column_pattern = sqlCharToString(column, column_len);
    bool metadata_id = conn_->getMetadataId();

    if (schema_pattern.empty()) {
        const auto parsed_table = parsePrivilegeObjectPath(table_pattern);
        if (!parsed_table.schema.empty()) {
            schema_pattern = parsed_table.schema;
            if (!parsed_table.table.empty()) {
                table_pattern = parsed_table.table;
            }
            if (parsed_table.has_column && parsed_table.column.empty() == false) {
                if (column_pattern.empty()) {
                    column_pattern = parsed_table.column;
                }
            }
        }
    }

    std::vector<ColumnMetadata> cols;
    cols.push_back(makeCatalogColumn("TABLE_CAT", SQL_VARCHAR, DriverConfig::MAX_CATALOG_NAME_LEN));
    cols.push_back(makeCatalogColumn("TABLE_SCHEM", SQL_VARCHAR, DriverConfig::MAX_SCHEMA_NAME_LEN));
    cols.push_back(makeCatalogColumn("TABLE_NAME", SQL_VARCHAR, DriverConfig::MAX_TABLE_NAME_LEN));
    cols.push_back(makeCatalogColumn("COLUMN_NAME", SQL_VARCHAR, DriverConfig::MAX_COLUMN_NAME_LEN));
    cols.push_back(makeCatalogColumn("GRANTOR", SQL_VARCHAR, 64));
    cols.push_back(makeCatalogColumn("GRANTEE", SQL_VARCHAR, 64));
    cols.push_back(makeCatalogColumn("PRIVILEGE", SQL_VARCHAR, 64));
    cols.push_back(makeCatalogColumn("IS_GRANTABLE", SQL_VARCHAR, 3));

    const std::string& current_catalog = conn_->getCurrentDatabase();

    if (!matchPattern(current_catalog, catalog_pattern, metadata_id)) {
        setCatalogResult(std::move(cols), {});
        return SQL_SUCCESS;
    }

    std::vector<std::vector<std::string>> grant_rows;
    auto grant_status = queryShowGrants(conn_, grant_rows);
    if (grant_status != SQL_SUCCESS) {
        setError("HY000", 0, "Failed to execute SHOW GRANTS");
        return grant_status;
    }

    std::vector<std::vector<std::string>> rows;
    for (const auto& grant_row : grant_rows) {
        if (grant_row.size() < 5) {
            continue;
        }
        if (isRoleGrantObject(grant_row[1], grant_row[2])) {
            continue;
        }

        const auto path = parsePrivilegeObjectPath(grant_row[1]);
        if (!path.has_column) {
            continue;
        }
        if (path.schema.empty()) {
            continue;
        }
        if (!matchSchemaPattern(path, schema_pattern, metadata_id)) {
            continue;
        }
        if (!matchTablePattern(path, table_pattern, metadata_id)) {
            continue;
        }
        if (!matchPattern(path.column, column_pattern, metadata_id)) {
            continue;
        }

        rows.push_back({
            current_catalog,
            path.schema,
            path.table,
            path.column,
            grant_row[3],
            grant_row[0],
            grant_row[2],
            normalizeGrantOption(grant_row[4])
        });
    }

    setCatalogResult(std::move(cols), std::move(rows));
    return SQL_SUCCESS;
}

// =============================================================================
// OdbcDescriptor Implementation
// =============================================================================

OdbcDescriptor::OdbcDescriptor(OdbcConnection* conn, DescriptorType type, bool implicit)
    : conn_(conn), desc_type_(type), implicit_(implicit) {
    alloc_type_ = implicit ? SQL_DESC_ALLOC_AUTO : SQL_DESC_ALLOC_USER;
}

OdbcDescriptor::~OdbcDescriptor() = default;

SQLRETURN OdbcDescriptor::setField(SQLSMALLINT rec_number, SQLSMALLINT field_identifier,
                                    SQLPOINTER value, SQLINTEGER buffer_length) {
    clearDiagnostics();

    if (rec_number < 0) {
        setError("07009", 0, "Invalid descriptor index");
        return SQL_ERROR;
    }

    auto requireValue = [&](const char* context) -> bool {
        if (!value) {
            setError("HY009", 0, std::string("Invalid use of null pointer: ") + context);
            return false;
        }
        return true;
    };

    auto copyStringField = [&](std::string& target) {
        if (!value) {
            target.clear();
            return;
        }
        if (buffer_length < 0 || buffer_length == SQL_NTS) {
            target = static_cast<const char*>(value);
        } else {
            target.assign(static_cast<const char*>(value),
                          static_cast<const char*>(value) + buffer_length);
        }
    };

    if (rec_number > 0) {
        auto rec_index = static_cast<size_t>(rec_number - 1);
        while (records_.size() <= rec_index) {
            records_.emplace_back();
        }
        if (rec_number > count_) {
            count_ = rec_number;
        }
    }

    // Header fields
    if (rec_number == 0) {
        switch (field_identifier) {
            case SQL_DESC_COUNT:
                if (!requireValue("SQL_DESC_COUNT")) {
                    return SQL_ERROR;
                }
                count_ = *static_cast<SQLSMALLINT*>(value);
                break;
            case SQL_DESC_ALLOC_TYPE:
                // Read-only
                break;
            case SQL_DESC_ARRAY_SIZE:
                if (!requireValue("SQL_DESC_ARRAY_SIZE")) {
                    return SQL_ERROR;
                }
                array_size_ = *static_cast<SQLULEN*>(value);
                break;
            case SQL_DESC_ARRAY_STATUS_PTR:
                array_status_ptr_ = static_cast<SQLULEN*>(value);
                break;
            case SQL_DESC_BIND_OFFSET_PTR:
                bind_offset_ptr_ = static_cast<SQLLEN*>(value);
                break;
            case SQL_DESC_BIND_TYPE:
                if (!requireValue("SQL_DESC_BIND_TYPE")) {
                    return SQL_ERROR;
                }
                bind_type_ = *static_cast<SQLULEN*>(value);
                break;
            case SQL_DESC_ROWS_PROCESSED_PTR:
                rows_processed_ptr_ = static_cast<SQLULEN*>(value);
                break;
            default:
                setError("HY091", 0, "Invalid descriptor field identifier");
                return SQL_ERROR;
        }
        return SQL_SUCCESS;
    }

    auto& rec = records_[rec_number - 1];

    switch (field_identifier) {
        case SQL_DESC_TYPE:
            if (!requireValue("SQL_DESC_TYPE")) {
                return SQL_ERROR;
            }
            rec.type = *static_cast<SQLSMALLINT*>(value);
            break;
        case SQL_DESC_CONCISE_TYPE:
            if (!requireValue("SQL_DESC_CONCISE_TYPE")) {
                return SQL_ERROR;
            }
            rec.concise_type = *static_cast<SQLSMALLINT*>(value);
            break;
        case SQL_DESC_LENGTH:
            if (!requireValue("SQL_DESC_LENGTH")) {
                return SQL_ERROR;
            }
            rec.length = *static_cast<SQLLEN*>(value);
            break;
        case SQL_DESC_OCTET_LENGTH:
            if (!requireValue("SQL_DESC_OCTET_LENGTH")) {
                return SQL_ERROR;
            }
            rec.octet_length = *static_cast<SQLLEN*>(value);
            break;
        case SQL_DESC_PRECISION:
            if (!requireValue("SQL_DESC_PRECISION")) {
                return SQL_ERROR;
            }
            rec.precision = *static_cast<SQLSMALLINT*>(value);
            break;
        case SQL_DESC_SCALE:
            if (!requireValue("SQL_DESC_SCALE")) {
                return SQL_ERROR;
            }
            rec.scale = *static_cast<SQLSMALLINT*>(value);
            break;
        case SQL_DESC_UNSIGNED:
            if (!requireValue("SQL_DESC_UNSIGNED")) {
                return SQL_ERROR;
            }
            rec.unsigned_ = *static_cast<SQLSMALLINT*>(value);
            break;
        case SQL_DESC_FIXED_PREC_SCALE:
            if (!requireValue("SQL_DESC_FIXED_PREC_SCALE")) {
                return SQL_ERROR;
            }
            rec.fixed_prec_scale = *static_cast<SQLSMALLINT*>(value);
            break;
        case SQL_DESC_AUTO_UNIQUE_VALUE:
            if (!requireValue("SQL_DESC_AUTO_UNIQUE_VALUE")) {
                return SQL_ERROR;
            }
            rec.auto_unique_value = *static_cast<SQLSMALLINT*>(value);
            break;
        case SQL_DESC_CASE_SENSITIVE:
            if (!requireValue("SQL_DESC_CASE_SENSITIVE")) {
                return SQL_ERROR;
            }
            rec.case_sensitive = *static_cast<SQLSMALLINT*>(value);
            break;
        case SQL_DESC_SEARCHABLE:
            if (!requireValue("SQL_DESC_SEARCHABLE")) {
                return SQL_ERROR;
            }
            rec.searchable = *static_cast<SQLSMALLINT*>(value);
            break;
        case SQL_DESC_UPDATABLE:
            if (!requireValue("SQL_DESC_UPDATABLE")) {
                return SQL_ERROR;
            }
            rec.updatable = *static_cast<SQLSMALLINT*>(value);
            break;
        case SQL_DESC_NUM_PREC_RADIX:
            if (!requireValue("SQL_DESC_NUM_PREC_RADIX")) {
                return SQL_ERROR;
            }
            rec.num_prec_radix = *static_cast<SQLSMALLINT*>(value);
            break;
        case SQL_DESC_DISPLAY_SIZE:
            if (!requireValue("SQL_DESC_DISPLAY_SIZE")) {
                return SQL_ERROR;
            }
            rec.display_size = *static_cast<SQLLEN*>(value);
            break;
        case SQL_DESC_DATETIME_INTERVAL_CODE:
            if (!requireValue("SQL_DESC_DATETIME_INTERVAL_CODE")) {
                return SQL_ERROR;
            }
            rec.datetime_interval_code = *static_cast<SQLSMALLINT*>(value);
            break;
        case SQL_DESC_DATETIME_INTERVAL_PRECISION:
            if (!requireValue("SQL_DESC_DATETIME_INTERVAL_PRECISION")) {
                return SQL_ERROR;
            }
            rec.datetime_interval_precision = *static_cast<SQLSMALLINT*>(value);
            break;
        case SQL_DESC_MAXIMUM_SCALE:
            if (!requireValue("SQL_DESC_MAXIMUM_SCALE")) {
                return SQL_ERROR;
            }
            rec.maximum_scale = *static_cast<SQLSMALLINT*>(value);
            break;
        case SQL_DESC_MINIMUM_SCALE:
            if (!requireValue("SQL_DESC_MINIMUM_SCALE")) {
                return SQL_ERROR;
            }
            rec.minimum_scale = *static_cast<SQLSMALLINT*>(value);
            break;
        case SQL_DESC_PARAMETER_TYPE:
            if (!requireValue("SQL_DESC_PARAMETER_TYPE")) {
                return SQL_ERROR;
            }
            rec.concise_type = *static_cast<SQLSMALLINT*>(value);
            break;
        case SQL_DESC_NAME:
            copyStringField(rec.name);
            break;
        case SQL_DESC_LABEL:
            copyStringField(rec.label);
            break;
        case SQL_DESC_TYPE_NAME:
            copyStringField(rec.type_name);
            break;
        case SQL_DESC_TABLE_NAME:
            copyStringField(rec.table_name);
            break;
        case SQL_DESC_SCHEMA_NAME:
            copyStringField(rec.schema_name);
            break;
        case SQL_DESC_CATALOG_NAME:
            copyStringField(rec.catalog_name);
            break;
        case SQL_DESC_BASE_COLUMN_NAME:
            copyStringField(rec.base_column_name);
            break;
        case SQL_DESC_BASE_TABLE_NAME:
            copyStringField(rec.base_table_name);
            break;
        case SQL_DESC_LITERAL_PREFIX:
            copyStringField(rec.literal_prefix);
            break;
        case SQL_DESC_LITERAL_SUFFIX:
            copyStringField(rec.literal_suffix);
            break;
        case SQL_DESC_LOCAL_TYPE_NAME:
            copyStringField(rec.local_type_name);
            break;
        case SQL_DESC_UNNAMED:
            if (!requireValue("SQL_DESC_UNNAMED")) {
                return SQL_ERROR;
            }
            rec.unnamed = *static_cast<SQLSMALLINT*>(value);
            break;
        case SQL_DESC_DATA_PTR:
            rec.data_ptr = value;
            break;
        case SQL_DESC_INDICATOR_PTR:
            rec.indicator_ptr = static_cast<SQLLEN*>(value);
            break;
        case SQL_DESC_OCTET_LENGTH_PTR:
            rec.octet_length_ptr = static_cast<SQLLEN*>(value);
            break;
        case SQL_DESC_NULLABLE:
            if (!requireValue("SQL_DESC_NULLABLE")) {
                return SQL_ERROR;
            }
            rec.nullable = *static_cast<SQLSMALLINT*>(value);
            break;
        default:
            setError("HY091", 0, "Invalid descriptor field identifier");
            return SQL_ERROR;
    }

    return SQL_SUCCESS;
}

SQLRETURN OdbcDescriptor::getField(SQLSMALLINT rec_number, SQLSMALLINT field_identifier,
                                    SQLPOINTER value, SQLINTEGER buffer_length,
                                    SQLINTEGER* string_length) {
    clearDiagnostics();

    auto copyString = [&](const std::string& source) -> SQLRETURN {
        if (string_length) {
            *string_length = static_cast<SQLINTEGER>(source.size());
        }
        if (!value || buffer_length <= 0) {
            return SQL_SUCCESS;
        }
        auto copy_len = static_cast<size_t>(buffer_length - 1);
        auto actual_len = std::min(copy_len, source.size());
        if (actual_len > 0) {
            std::memcpy(value, source.data(), actual_len);
        }
        static_cast<char*>(value)[actual_len] = '\0';
        if (source.size() >= static_cast<size_t>(buffer_length)) {
            setError("01004", 0, "String data, right truncated");
            return SQL_SUCCESS_WITH_INFO;
        }
        return SQL_SUCCESS;
    };

    if (rec_number == 0) {
        switch (field_identifier) {
            case SQL_DESC_COUNT:
                if (value) *static_cast<SQLSMALLINT*>(value) = count_;
                if (string_length) *string_length = sizeof(SQLSMALLINT);
                break;
            case SQL_DESC_ALLOC_TYPE:
                if (value) *static_cast<SQLSMALLINT*>(value) = alloc_type_;
                if (string_length) *string_length = sizeof(SQLSMALLINT);
                break;
            case SQL_DESC_ARRAY_SIZE:
                if (value) *static_cast<SQLULEN*>(value) = array_size_;
                if (string_length) *string_length = sizeof(SQLULEN);
                break;
            case SQL_DESC_ARRAY_STATUS_PTR:
                if (value) *static_cast<SQLULEN*>(value) =
                    reinterpret_cast<SQLULEN>(array_status_ptr_);
                if (string_length) *string_length = sizeof(SQLPOINTER);
                break;
            case SQL_DESC_BIND_OFFSET_PTR:
                if (value) *static_cast<SQLLEN*>(value) = bind_offset_ptr_ ? *bind_offset_ptr_ : 0;
                if (string_length) *string_length = sizeof(SQLLEN);
                break;
            case SQL_DESC_BIND_TYPE:
                if (value) *static_cast<SQLULEN*>(value) = bind_type_;
                if (string_length) *string_length = sizeof(SQLULEN);
                break;
            case SQL_DESC_ROWS_PROCESSED_PTR:
                if (value) *static_cast<SQLULEN*>(value) =
                    reinterpret_cast<SQLULEN>(rows_processed_ptr_);
                if (string_length) *string_length = sizeof(SQLPOINTER);
                break;
            default:
                setError("HY091", 0, "Invalid descriptor field identifier");
                return SQL_ERROR;
        }
        return SQL_SUCCESS;
    }

    if (rec_number < 1 || rec_number > count_ ||
        static_cast<size_t>(rec_number) > records_.size()) {
        setError("07009", 0, "Invalid descriptor index");
        return SQL_ERROR;
    }

    const auto& rec = records_[rec_number - 1];

    switch (field_identifier) {
        case SQL_DESC_TYPE:
            if (value) *static_cast<SQLSMALLINT*>(value) = rec.type;
            if (string_length) *string_length = sizeof(SQLSMALLINT);
            break;
        case SQL_DESC_CONCISE_TYPE:
            if (value) *static_cast<SQLSMALLINT*>(value) = rec.concise_type;
            if (string_length) *string_length = sizeof(SQLSMALLINT);
            break;
        case SQL_DESC_LENGTH:
            if (value) *static_cast<SQLLEN*>(value) = rec.length;
            if (string_length) *string_length = sizeof(SQLLEN);
            break;
        case SQL_DESC_PRECISION:
            if (value) *static_cast<SQLSMALLINT*>(value) = rec.precision;
            if (string_length) *string_length = sizeof(SQLSMALLINT);
            break;
        case SQL_DESC_SCALE:
            if (value) *static_cast<SQLSMALLINT*>(value) = rec.scale;
            if (string_length) *string_length = sizeof(SQLSMALLINT);
            break;
        case SQL_DESC_OCTET_LENGTH:
            if (value) *static_cast<SQLLEN*>(value) = rec.octet_length;
            if (string_length) *string_length = sizeof(SQLLEN);
            break;
        case SQL_DESC_UNSIGNED:
            if (value) *static_cast<SQLSMALLINT*>(value) = rec.unsigned_;
            if (string_length) *string_length = sizeof(SQLSMALLINT);
            break;
        case SQL_DESC_FIXED_PREC_SCALE:
            if (value) *static_cast<SQLSMALLINT*>(value) = rec.fixed_prec_scale;
            if (string_length) *string_length = sizeof(SQLSMALLINT);
            break;
        case SQL_DESC_AUTO_UNIQUE_VALUE:
            if (value) *static_cast<SQLSMALLINT*>(value) = rec.auto_unique_value;
            if (string_length) *string_length = sizeof(SQLSMALLINT);
            break;
        case SQL_DESC_CASE_SENSITIVE:
            if (value) *static_cast<SQLSMALLINT*>(value) = rec.case_sensitive;
            if (string_length) *string_length = sizeof(SQLSMALLINT);
            break;
        case SQL_DESC_SEARCHABLE:
            if (value) *static_cast<SQLSMALLINT*>(value) = rec.searchable;
            if (string_length) *string_length = sizeof(SQLSMALLINT);
            break;
        case SQL_DESC_UPDATABLE:
            if (value) *static_cast<SQLSMALLINT*>(value) = rec.updatable;
            if (string_length) *string_length = sizeof(SQLSMALLINT);
            break;
        case SQL_DESC_DISPLAY_SIZE:
            if (value) *static_cast<SQLLEN*>(value) = rec.display_size;
            if (string_length) *string_length = sizeof(SQLLEN);
            break;
        case SQL_DESC_NUM_PREC_RADIX:
            if (value) *static_cast<SQLSMALLINT*>(value) = rec.num_prec_radix;
            if (string_length) *string_length = sizeof(SQLSMALLINT);
            break;
        case SQL_DESC_DATETIME_INTERVAL_CODE:
            if (value) *static_cast<SQLSMALLINT*>(value) = rec.datetime_interval_code;
            if (string_length) *string_length = sizeof(SQLSMALLINT);
            break;
        case SQL_DESC_DATETIME_INTERVAL_PRECISION:
            if (value) *static_cast<SQLSMALLINT*>(value) = rec.datetime_interval_precision;
            if (string_length) *string_length = sizeof(SQLSMALLINT);
            break;
        case SQL_DESC_MAXIMUM_SCALE:
            if (value) *static_cast<SQLSMALLINT*>(value) = rec.maximum_scale;
            if (string_length) *string_length = sizeof(SQLSMALLINT);
            break;
        case SQL_DESC_MINIMUM_SCALE:
            if (value) *static_cast<SQLSMALLINT*>(value) = rec.minimum_scale;
            if (string_length) *string_length = sizeof(SQLSMALLINT);
            break;
        case SQL_DESC_PARAMETER_TYPE:
            if (value) *static_cast<SQLSMALLINT*>(value) = rec.concise_type;
            if (string_length) *string_length = sizeof(SQLSMALLINT);
            break;
        case SQL_DESC_NAME:
            return copyString(rec.name);
        case SQL_DESC_LABEL:
            return copyString(rec.label);
        case SQL_DESC_TYPE_NAME:
            return copyString(rec.type_name);
        case SQL_DESC_TABLE_NAME:
            return copyString(rec.table_name);
        case SQL_DESC_SCHEMA_NAME:
            return copyString(rec.schema_name);
        case SQL_DESC_CATALOG_NAME:
            return copyString(rec.catalog_name);
        case SQL_DESC_BASE_COLUMN_NAME:
            return copyString(rec.base_column_name);
        case SQL_DESC_BASE_TABLE_NAME:
            return copyString(rec.base_table_name);
        case SQL_DESC_LITERAL_PREFIX:
            return copyString(rec.literal_prefix);
        case SQL_DESC_LITERAL_SUFFIX:
            return copyString(rec.literal_suffix);
        case SQL_DESC_LOCAL_TYPE_NAME:
            return copyString(rec.local_type_name);
        case SQL_DESC_DATA_PTR:
            if (value) *static_cast<SQLPOINTER*>(value) = rec.data_ptr;
            if (string_length) *string_length = sizeof(SQLPOINTER);
            break;
        case SQL_DESC_INDICATOR_PTR:
            if (value) *static_cast<SQLLEN*>(value) = rec.indicator_ptr ? *rec.indicator_ptr : 0;
            if (string_length) *string_length = sizeof(SQLLEN);
            break;
        case SQL_DESC_OCTET_LENGTH_PTR:
            if (value) *static_cast<SQLLEN*>(value) = rec.octet_length_ptr ? *rec.octet_length_ptr : 0;
            if (string_length) *string_length = sizeof(SQLLEN);
            break;
        case SQL_DESC_UNNAMED:
            if (value) *static_cast<SQLSMALLINT*>(value) = rec.unnamed;
            if (string_length) *string_length = sizeof(SQLSMALLINT);
            break;
        case SQL_DESC_NULLABLE:
            if (value) *static_cast<SQLSMALLINT*>(value) = rec.nullable;
            if (string_length) *string_length = sizeof(SQLSMALLINT);
            break;
        default:
            setError("HY091", 0, "Invalid descriptor field identifier");
            return SQL_ERROR;
    }

    return SQL_SUCCESS;
}

SQLRETURN OdbcDescriptor::setRec(SQLSMALLINT rec_number, SQLSMALLINT type, SQLSMALLINT sub_type,
                                  SQLLEN length, SQLSMALLINT precision, SQLSMALLINT scale,
                                  SQLPOINTER data, SQLLEN* string_length, SQLLEN* indicator) {
    clearDiagnostics();

    if (rec_number < 1) {
        setError("07009", 0, "Invalid descriptor index");
        return SQL_ERROR;
    }

    auto rec_index = static_cast<size_t>(rec_number - 1);
    while (records_.size() <= rec_index) {
        records_.emplace_back();
    }
    if (rec_number > count_) {
        count_ = rec_number;
    }

    auto& rec = records_[rec_index];
    rec.type = type;
    rec.concise_type = type;
    rec.datetime_interval_code = sub_type;
    rec.length = length;
    rec.octet_length = length;
    rec.precision = precision;
    rec.scale = scale;
    rec.data_ptr = data;
    rec.octet_length_ptr = string_length;
    rec.indicator_ptr = indicator;

    return SQL_SUCCESS;
}

SQLRETURN OdbcDescriptor::getRec(SQLSMALLINT rec_number, SQLCHAR* name, SQLSMALLINT buffer_length,
                                  SQLSMALLINT* string_length, SQLSMALLINT* type,
                                  SQLSMALLINT* sub_type, SQLLEN* length, SQLSMALLINT* precision,
                                  SQLSMALLINT* scale, SQLSMALLINT* nullable) {
    clearDiagnostics();

    if (rec_number < 1 || rec_number > count_ ||
        static_cast<size_t>(rec_number) > records_.size()) {
        setError("07009", 0, "Invalid descriptor index");
        return SQL_ERROR;
    }

    const auto& rec = records_[rec_number - 1];
    SQLRETURN result = SQL_SUCCESS;

    // Copy name
    if (string_length) {
        *string_length = static_cast<SQLSMALLINT>(rec.name.size());
    }
    if (name && buffer_length > 0) {
        size_t copy_len = std::min(static_cast<size_t>(buffer_length - 1), rec.name.size());
        std::memcpy(name, rec.name.c_str(), copy_len);
        name[copy_len] = '\0';
        if (rec.name.size() >= static_cast<size_t>(buffer_length)) {
            setError("01004", 0, "String data, right truncated");
            result = SQL_SUCCESS_WITH_INFO;
        }
    }

    if (type) *type = rec.type;
    if (sub_type) *sub_type = rec.datetime_interval_code;
    if (length) *length = rec.length;
    if (precision) *precision = rec.precision;
    if (scale) *scale = rec.scale;
    if (nullable) *nullable = rec.nullable;

    return result;
}

SQLRETURN OdbcDescriptor::copyDesc(OdbcDescriptor* target) {
    clearDiagnostics();

    if (!target) {
        setError("HY009", 0, "Invalid use of null pointer");
        return SQL_ERROR;
    }

    target->count_ = count_;
    target->array_size_ = array_size_;
    target->alloc_type_ = alloc_type_;
    target->array_status_ptr_ = array_status_ptr_;
    target->bind_offset_ptr_ = bind_offset_ptr_;
    target->bind_type_ = bind_type_;
    target->rows_processed_ptr_ = rows_processed_ptr_;
    target->records_ = records_;

    return SQL_SUCCESS;
}

void OdbcDescriptor::resetDescriptor() {
    count_ = 0;
    records_.clear();
}

}  // namespace odbc
}  // namespace scratchbird
