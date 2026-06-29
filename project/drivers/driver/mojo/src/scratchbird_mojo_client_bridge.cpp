// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/client/scratchbird_client.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace {

std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (unsigned char ch : value) {
        switch (ch) {
            case '\"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (ch < 0x20) {
                    out << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                        << static_cast<int>(ch) << std::dec;
                } else {
                    out << static_cast<char>(ch);
                }
        }
    }
    return out.str();
}

std::string quote(const std::string& value) {
    return "\"" + jsonEscape(value) + "\"";
}

bool writeText(const char* path, const std::string& text) {
    if (!path || !*path) {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << text;
    return static_cast<bool>(out);
}

void writeBridgeError(const char* path, const sb_error& err, const char* operation) {
    std::ostringstream out;
    out << "{"
        << "\"operation\":" << quote(operation ? operation : "unknown") << ","
        << "\"code\":" << static_cast<int>(err.code) << ","
        << "\"message\":" << quote(err.message)
        << "}\n";
    writeText(path, out.str());
}

std::string hexBytes(const uint8_t* data, size_t length) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (size_t index = 0; index < length; ++index) {
        const uint8_t value = data[index];
        out.push_back(kHex[(value >> 4) & 0x0F]);
        out.push_back(kHex[value & 0x0F]);
    }
    return out;
}

std::string jsonValue(sb_value& value) {
    if (value.is_null) {
        return "null";
    }
    switch (value.type) {
        case SB_TYPE_BOOLEAN:
            return value.data.boolean_val ? "true" : "false";
        case SB_TYPE_SMALLINT:
            return std::to_string(value.data.smallint_val);
        case SB_TYPE_INTEGER:
            return std::to_string(value.data.integer_val);
        case SB_TYPE_BIGINT:
            return std::to_string(value.data.bigint_val);
        case SB_TYPE_REAL:
            return std::to_string(value.data.real_val);
        case SB_TYPE_DOUBLE:
            return std::to_string(value.data.double_val);
        case SB_TYPE_DATE: {
            std::ostringstream out;
            out << std::setw(4) << std::setfill('0') << value.data.date_val.year << '-'
                << std::setw(2) << std::setfill('0') << value.data.date_val.month << '-'
                << std::setw(2) << std::setfill('0') << value.data.date_val.day;
            return quote(out.str());
        }
        case SB_TYPE_TIME:
        case SB_TYPE_TIME_TZ: {
            std::ostringstream out;
            out << std::setw(2) << std::setfill('0') << value.data.time_val.hour << ':'
                << std::setw(2) << std::setfill('0') << value.data.time_val.minute << ':'
                << std::setw(2) << std::setfill('0') << value.data.time_val.second;
            if (value.data.time_val.microsecond != 0) {
                out << '.' << std::setw(6) << std::setfill('0') << value.data.time_val.microsecond;
            }
            return quote(out.str());
        }
        case SB_TYPE_TIMESTAMP:
        case SB_TYPE_TIMESTAMP_TZ:
            return std::to_string(value.data.timestamp_val.epoch_microseconds);
        case SB_TYPE_UUID:
            return quote(hexBytes(value.data.uuid_val.bytes, 16));
        case SB_TYPE_BLOB:
            return quote(hexBytes(value.data.binary_val.data, value.data.binary_val.length));
        default:
            if (value.data.string_val.data) {
                return quote(std::string(value.data.string_val.data, value.data.string_val.length));
            }
            return quote("");
    }
}

std::string resultToJson(sb_result* result, sb_error& err) {
    std::ostringstream out;
    out << "{\"columns\":[";
    const int column_count = sb_column_count(result);
    for (int index = 0; index < column_count; ++index) {
        sb_column_meta meta{};
        if (sb_get_column_meta(result, index, &meta) != 0) {
            if (index != 0) {
                out << ',';
            }
            out << "{\"name\":\"column_" << index << "\",\"type\":99}";
            continue;
        }
        if (index != 0) {
            out << ',';
        }
        out << "{\"name\":" << quote(meta.name ? meta.name : "")
            << ",\"type\":" << static_cast<int>(meta.type)
            << ",\"nullable\":" << (meta.nullable ? "true" : "false") << "}";
    }
    out << "],\"rows\":[";
    bool first_row = true;
    int64_t row_count = 0;
    sb_row row{};
    while (sb_fetch(result, &row, &err) == SB_OK) {
        if (!first_row) {
            out << ',';
        }
        first_row = false;
        out << '[';
        for (int column = 0; column < column_count; ++column) {
            if (column != 0) {
                out << ',';
            }
            sb_value value{};
            if (sb_value_get(&row, column, &value) != 0) {
                out << "null";
            } else {
                out << jsonValue(value);
            }
        }
        out << ']';
        ++row_count;
    }
    out << "],\"row_count\":" << row_count
        << ",\"rows_affected\":" << sb_rows_affected(result)
        << "}\n";
    return out.str();
}

sb_connection* asConnection(uint64_t handle) {
    return reinterpret_cast<sb_connection*>(static_cast<uintptr_t>(handle));
}

} // namespace

extern "C" {

uint64_t sb_mojo_bridge_connect(const char* dsn, const char* error_path) {
    sb_error err{};
    sb_connection* conn = sb_connect(dsn, &err);
    if (!conn) {
        writeBridgeError(error_path, err, "connect");
        return 0;
    }
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(conn));
}

int sb_mojo_bridge_disconnect(uint64_t handle) {
    sb_connection* conn = asConnection(handle);
    if (!conn) {
        return 1;
    }
    sb_disconnect(conn);
    return 0;
}

int sb_mojo_bridge_execute_to_file(uint64_t handle,
                                   const char* sql,
                                   const char* result_path,
                                   const char* error_path) {
    sb_connection* conn = asConnection(handle);
    if (!conn || !sql) {
        sb_error err{};
        err.code = SB_ERR_INVALID_HANDLE;
        std::snprintf(err.message, sizeof(err.message), "invalid Mojo bridge execution handle");
        writeBridgeError(error_path, err, "execute");
        return 1;
    }
    sb_error err{};
    sb_result* result = sb_execute(conn, sql, &err);
    if (!result) {
        writeBridgeError(error_path, err, "execute");
        return 1;
    }
    const std::string payload = resultToJson(result, err);
    sb_result_free(result);
    if (!writeText(result_path, payload)) {
        sb_error write_err{};
        write_err.code = SB_ERR_INVALID_PARAM;
        std::snprintf(write_err.message, sizeof(write_err.message), "could not write Mojo bridge result file");
        writeBridgeError(error_path, write_err, "execute");
        return 1;
    }
    return 0;
}

int sb_mojo_bridge_metadata_to_file(uint64_t handle,
                                    const char* collection,
                                    const char* result_path,
                                    const char* error_path) {
    sb_connection* conn = asConnection(handle);
    if (!conn || !collection) {
        sb_error err{};
        err.code = SB_ERR_INVALID_HANDLE;
        std::snprintf(err.message, sizeof(err.message), "invalid Mojo bridge metadata handle");
        writeBridgeError(error_path, err, "metadata");
        return 1;
    }
    sb_error err{};
    sb_result* result = sb_metadata_query(conn, collection, &err);
    if (!result) {
        writeBridgeError(error_path, err, "metadata");
        return 1;
    }
    const std::string payload = resultToJson(result, err);
    sb_result_free(result);
    if (!writeText(result_path, payload)) {
        sb_error write_err{};
        write_err.code = SB_ERR_INVALID_PARAM;
        std::snprintf(write_err.message, sizeof(write_err.message), "could not write Mojo bridge metadata file");
        writeBridgeError(error_path, write_err, "metadata");
        return 1;
    }
    return 0;
}

int sb_mojo_bridge_copy_to_file(uint64_t handle,
                                const char* sql,
                                const char* data,
                                const char* result_path,
                                const char* error_path) {
    sb_connection* conn = asConnection(handle);
    if (!conn || !sql || !data) {
        sb_error err{};
        err.code = SB_ERR_INVALID_HANDLE;
        std::snprintf(err.message, sizeof(err.message), "invalid Mojo bridge COPY handle");
        writeBridgeError(error_path, err, "copy");
        return 1;
    }
    sb_error err{};
    int64_t rows_affected = 0;
    const int rc = sb_execute_copy_from_buffer(conn,
                                               sql,
                                               data,
                                               std::strlen(data),
                                               &rows_affected,
                                               &err);
    if (rc != SB_OK) {
        writeBridgeError(error_path, err, "copy");
        return rc == 0 ? 1 : rc;
    }
    std::ostringstream out;
    out << "{\"columns\":[],\"rows\":[],\"row_count\":0,\"rows_affected\":"
        << rows_affected << "}\n";
    if (!writeText(result_path, out.str())) {
        sb_error write_err{};
        write_err.code = SB_ERR_INVALID_PARAM;
        std::snprintf(write_err.message, sizeof(write_err.message), "could not write Mojo bridge COPY result file");
        writeBridgeError(error_path, write_err, "copy");
        return 1;
    }
    return 0;
}

int sb_mojo_bridge_tx_begin(uint64_t handle, const char* error_path) {
    sb_error err{};
    const int rc = sb_tx_begin(asConnection(handle), &err);
    if (rc != 0) {
        writeBridgeError(error_path, err, "begin");
    }
    return rc;
}

int sb_mojo_bridge_tx_commit(uint64_t handle, const char* error_path) {
    sb_error err{};
    const int rc = sb_tx_commit(asConnection(handle), &err);
    if (rc != 0) {
        writeBridgeError(error_path, err, "commit");
    }
    return rc;
}

int sb_mojo_bridge_tx_rollback(uint64_t handle, const char* error_path) {
    sb_error err{};
    const int rc = sb_tx_rollback(asConnection(handle), &err);
    if (rc != 0) {
        writeBridgeError(error_path, err, "rollback");
    }
    return rc;
}

int sb_mojo_bridge_tx_savepoint(uint64_t handle, const char* name, const char* error_path) {
    sb_error err{};
    const int rc = sb_tx_savepoint(asConnection(handle), name, &err);
    if (rc != 0) {
        writeBridgeError(error_path, err, "savepoint");
    }
    return rc;
}

int sb_mojo_bridge_tx_release_savepoint(uint64_t handle, const char* name, const char* error_path) {
    sb_error err{};
    const int rc = sb_tx_release_savepoint(asConnection(handle), name, &err);
    if (rc != 0) {
        writeBridgeError(error_path, err, "release_savepoint");
    }
    return rc;
}

int sb_mojo_bridge_tx_rollback_to(uint64_t handle, const char* name, const char* error_path) {
    sb_error err{};
    const int rc = sb_tx_rollback_to(asConnection(handle), name, &err);
    if (rc != 0) {
        writeBridgeError(error_path, err, "rollback_to");
    }
    return rc;
}

}
