// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <memory>
#include <iosfwd>
#include <string>
#include <vector>

#include "scratchbird/client/network_client.h"
#include "scratchbird/client/scratchbird_client.h"
#include "scratchbird/core/error_context.h"
#include "scratchbird/core/status.h"

namespace scratchbird {
namespace client {

struct ConnectionConfig {
    std::string database_name;
    std::string username;
    std::string password;
    std::string protocol{"native"};
    std::string role;
    std::string current_schema;
    std::string application_name{"scratchbird_driver"};
    std::string ssl_mode{"require"};
    std::string ssl_cert;
    std::string ssl_key;
    std::string ssl_root_cert;

    // inet_listener | local_ipc | managed
    std::string transport_mode{"inet_listener"};
    std::string host{"127.0.0.1"};
    uint16_t tcp_port{3092};
    std::string ipc_method{"auto"};
    std::string ipc_path;
    std::string front_door_mode{"direct"};

    std::string manager_auth_token;
    std::string manager_username;
    std::string manager_database;
    std::string manager_connection_profile{"SBsql"};
    std::string manager_client_intent{"SBsql"};
    uint16_t manager_client_flags{0};
    bool manager_auth_fast_path{true};

    uint16_t connect_client_flags{0x0100};
    std::string auth_method_id;
    std::string auth_token;
    std::string auth_method_payload;
    std::string auth_payload_json;
    std::string auth_payload_b64;
    std::string auth_provider_profile;
    std::vector<std::string> auth_required_methods;
    std::vector<std::string> auth_forbidden_methods;
    bool auth_require_channel_binding{false};
    std::string workload_identity_token;
    std::string proxy_principal_assertion;

    uint32_t connect_timeout_ms{5000};
    uint32_t query_timeout_ms{30000};
    uint32_t read_timeout_ms{30000};
    uint32_t write_timeout_ms{30000};
    uint32_t copy_window_bytes{65536};
    uint32_t copy_chunk_bytes{16384};
    bool enable_copy_streaming{false};
    bool binary_transfer{true};
    bool enable_compression{false};

    bool auto_commit{true};

    ConnectionConfig() = default;
    explicit ConnectionConfig(const std::string& db_name,
                              const std::string& user = "",
                              const std::string& pass = "")
        : database_name(db_name), username(user), password(pass) {}
};

core::Status parseConnectionConfig(const std::string& conn_str,
                                   ConnectionConfig* config,
                                   core::ErrorContext* ctx = nullptr);
core::Status probeAuthSurface(const std::string& conn_str,
                              AuthProbeResult* result,
                              core::ErrorContext* ctx = nullptr);

struct ColumnMeta {
    std::string name;
    sb_type type{SB_TYPE_UNKNOWN};
    uint32_t type_oid{0};
    int32_t type_modifier{0};
    size_t index{0};
    uint8_t format{0};
    bool nullable{true};
};

class ResultSetImpl;
class PreparedStatementImpl;

class ResultSet {
public:
    ResultSet();
    ~ResultSet();

    ResultSet(ResultSet&& other) noexcept;
    ResultSet& operator=(ResultSet&& other) noexcept;
    ResultSet(const ResultSet&) = delete;
    ResultSet& operator=(const ResultSet&) = delete;

    size_t getColumnCount() const;
    std::string getColumnName(size_t index) const;
    int getColumnIndex(const std::string& name) const;
    sb_type getColumnType(size_t index) const;
    uint32_t getColumnTypeOid(size_t index) const;
    uint8_t getColumnFormat(size_t index) const;
    bool isColumnNullable(size_t index) const;
    const std::vector<ColumnMeta>& getColumns() const;
    int64_t getRowCount() const;
    int64_t getRowsAffected() const;
    bool isEmpty() const;
    const std::string& getCommandTag() const;

    bool next();
    void reset();
    int64_t getCurrentRow() const;

    bool isNull(size_t column) const;
    bool getBool(size_t column) const;
    int16_t getInt16(size_t column) const;
    int32_t getInt32(size_t column) const;
    int64_t getInt64(size_t column) const;
    float getFloat(size_t column) const;
    double getDouble(size_t column) const;
    std::string getString(size_t column) const;
    std::vector<uint8_t> getBytes(size_t column) const;
    int64_t getTimestamp(size_t column) const;
    int32_t getDate(size_t column) const;
    int64_t getTime(size_t column) const;
    std::string getUUID(size_t column) const;
    const uint8_t* getRaw(size_t column, size_t* length) const;

    bool isNull(const std::string& column) const;
    bool getBool(const std::string& column) const;
    int16_t getInt16(const std::string& column) const;
    int32_t getInt32(const std::string& column) const;
    int64_t getInt64(const std::string& column) const;
    float getFloat(const std::string& column) const;
    double getDouble(const std::string& column) const;
    std::string getString(const std::string& column) const;
    std::vector<uint8_t> getBytes(const std::string& column) const;
    int64_t getTimestamp(const std::string& column) const;
    int32_t getDate(const std::string& column) const;
    int64_t getTime(const std::string& column) const;
    std::string getUUID(const std::string& column) const;

private:
    friend class Connection;
    friend class PreparedStatement;
    std::unique_ptr<ResultSetImpl> impl_;
};

class PreparedStatement {
public:
    PreparedStatement();
    ~PreparedStatement();

    PreparedStatement(PreparedStatement&& other) noexcept;
    PreparedStatement& operator=(PreparedStatement&& other) noexcept;
    PreparedStatement(const PreparedStatement&) = delete;
    PreparedStatement& operator=(const PreparedStatement&) = delete;

    size_t getParameterCount() const;
    bool isValid() const;
    void clearParameters();

    void setNull(size_t index);
    void setNull(size_t index, uint32_t type_oid);
    void setBool(size_t index, bool value);
    void setInt16(size_t index, int16_t value);
    void setInt32(size_t index, int32_t value);
    void setInt64(size_t index, int64_t value);
    void setFloat(size_t index, float value);
    void setDouble(size_t index, double value);
    void setString(size_t index, const std::string& value);
    void setString(size_t index, const std::string& value, uint32_t type_oid);
    void setBytes(size_t index, const std::vector<uint8_t>& value);
    void setBytes(size_t index, const uint8_t* data, size_t length);
    void setBinary(size_t index,
                   const uint8_t* data,
                   size_t length,
                   uint32_t type_oid,
                   bool length_prefixed);
    void setTimestamp(size_t index, int64_t microseconds);
    void setDate(size_t index, int32_t days);
    void setTime(size_t index, int64_t microseconds);
    void setUUID(size_t index, const std::vector<uint8_t>& value);
    void setUUID(size_t index, const std::string& value);

    core::Status executeQuery(ResultSet* results,
                              core::ErrorContext* ctx = nullptr);
    core::Status execute(int64_t* rows_affected = nullptr,
                         core::ErrorContext* ctx = nullptr);

private:
    friend class Connection;
    std::unique_ptr<PreparedStatementImpl> impl_;
};

enum class ConnectionState : uint8_t {
    DISCONNECTED = 0,
    CONNECTING = 1,
    CONNECTED = 2,
    IN_TRANSACTION = 3,
    ERROR_STATE = 4
};

class ConnectionImpl;

class Connection {
public:
    Connection();
    ~Connection();

    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    core::Status connect(const std::string& database,
                         const std::string& username = "",
                         const std::string& password = "",
                         core::ErrorContext* ctx = nullptr);
    core::Status connect(const ConnectionConfig& config,
                         core::ErrorContext* ctx = nullptr);
    void disconnect();
    bool isConnected() const;
    ConnectionState getState() const;
    std::string getLastError() const;
    ResolvedAuthContext getResolvedAuthContext() const;

    core::Status executeQuery(const std::string& sql,
                              ResultSet* results,
                              core::ErrorContext* ctx = nullptr);
    core::Status executeQuery(const std::string& sql,
                              ResultSet* results,
                              uint8_t flags,
                              core::ErrorContext* ctx = nullptr);
    core::Status executeSblr(uint64_t sblr_hash,
                             const std::vector<uint8_t>& sblr_bytecode,
                             ResultSet* results,
                             core::ErrorContext* ctx = nullptr);
    bool takeLastSblrCompiled(protocol::SblrCompiled* out);
    core::Status execute(const std::string& sql,
                         int64_t* rows_affected = nullptr,
                         core::ErrorContext* ctx = nullptr);
    core::Status prepare(const std::string& sql,
                         PreparedStatement* stmt,
                         core::ErrorContext* ctx = nullptr);
    core::Status metadataQuery(const std::string& collection_name,
                               ResultSet* results,
                               core::ErrorContext* ctx = nullptr);
    core::Status schemas(ResultSet* results,
                         const std::string& schema_pattern = "",
                         core::ErrorContext* ctx = nullptr);
    core::Status tables(ResultSet* results,
                        const std::string& schema_pattern = "",
                        const std::string& table_pattern = "",
                        core::ErrorContext* ctx = nullptr);
    core::Status columns(ResultSet* results,
                         const std::string& schema_pattern = "",
                         const std::string& table_pattern = "",
                         core::ErrorContext* ctx = nullptr);
    core::Status indexes(ResultSet* results,
                         const std::string& schema_pattern = "",
                         const std::string& table_pattern = "",
                         core::ErrorContext* ctx = nullptr);
    core::Status metadataSchemaPayload(const std::string* schema_pattern,
                                       bool expand_schema_parents,
                                       std::string* payload_json,
                                       core::ErrorContext* ctx = nullptr);
    core::Status attachCreate(const std::string& mode,
                              const std::string& db_name,
                              core::ErrorContext* ctx = nullptr);

    static bool supportsPreparedTransactions();
    static bool supportsDormantReattach();
    static bool supportsPortalResume();
    static core::Status buildPreparedTransactionSql(const std::string& verb,
                                                    const std::string& global_transaction_id,
                                                    std::string* sql,
                                                    core::ErrorContext* ctx = nullptr);

    core::Status beginTransaction(core::ErrorContext* ctx = nullptr);
    core::Status commit(core::ErrorContext* ctx = nullptr);
    core::Status rollback(core::ErrorContext* ctx = nullptr);
    core::Status prepareTransaction(const std::string& global_transaction_id,
                                    core::ErrorContext* ctx = nullptr);
    core::Status commitPrepared(const std::string& global_transaction_id,
                                core::ErrorContext* ctx = nullptr);
    core::Status rollbackPrepared(const std::string& global_transaction_id,
                                  core::ErrorContext* ctx = nullptr);
    core::Status detachToDormant(core::ErrorContext* ctx = nullptr);
    core::Status reattachDormant(const std::string& dormant_id,
                                 const std::string& auth_token,
                                 core::ErrorContext* ctx = nullptr);
    core::Status savepoint(const std::string& name,
                           core::ErrorContext* ctx = nullptr);
    core::Status releaseSavepoint(const std::string& name,
                                  core::ErrorContext* ctx = nullptr);
    core::Status rollbackTo(const std::string& name,
                            core::ErrorContext* ctx = nullptr);

    void setAutoCommit(bool enabled);
    bool getAutoCommit() const;
    bool inTransaction() const;
    uint64_t currentTransactionId() const;
    std::string getParameterStatus(const std::string& name) const;
    const ConnectionConfig& getConfig() const;
    void setCopyInputStream(std::istream* in);
    void setCopyInputSizeHintBytes(uint64_t bytes);
    void setCopyPreallocationFactorPercent(uint64_t percent);
    void setCopyOutputStream(std::ostream* out);

private:
    std::unique_ptr<ConnectionImpl> impl_;
};

} // namespace client
} // namespace scratchbird
