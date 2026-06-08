// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/**
 * @file odbc_handles.h
 * @brief ODBC Handle Classes
 *
 * Implements Environment, Connection, Statement, and Descriptor handles
 * for the ScratchBird ODBC driver.
 *
 * Part of Phase 3.8: ODBC Driver
 */

#ifndef SCRATCHBIRD_ODBC_HANDLES_H
#define SCRATCHBIRD_ODBC_HANDLES_H

#include "scratchbird/odbc/odbc_types.h"
#include "scratchbird/odbc/circuit_breaker.h"
#include "scratchbird/odbc/keepalive.h"
#include "scratchbird/odbc/leak_detector.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace scratchbird {
namespace odbc {

// Forward declarations
class OdbcEnvironment;
class OdbcConnection;
class OdbcStatement;
class OdbcDescriptor;
class OdbcClientBridge;

bool supportsPreparedTransactions();
bool supportsDormantReattach();
bool supportsPortalResume();
SQLRETURN buildPreparedTransactionSql(const std::string& verb,
                                      const std::string& global_transaction_id,
                                      std::string& out_sql,
                                      std::string* sqlstate_out = nullptr,
                                      std::string* message_out = nullptr);
SQLRETURN rejectDormantReattach(const char* operation,
                                std::string* sqlstate_out = nullptr,
                                std::string* message_out = nullptr);

// =============================================================================
// Handle Type Constants (for runtime type checking)
// =============================================================================

enum class HandleType : uint8_t {
    ENVIRONMENT = 1,
    CONNECTION = 2,
    STATEMENT = 3,
    DESCRIPTOR = 4
};

// =============================================================================
// Base Handle Class
// =============================================================================

/**
 * @brief Base class for all ODBC handles
 */
class OdbcHandle {
public:
    virtual ~OdbcHandle() = default;

    /**
     * @brief Get handle type
     */
    virtual HandleType getType() const = 0;

    /**
     * @brief Add diagnostic record
     */
    void addDiagnostic(const DiagnosticRecord& record);

    /**
     * @brief Clear all diagnostics
     */
    void clearDiagnostics();

    /**
     * @brief Get diagnostic count
     */
    SQLSMALLINT getDiagnosticCount() const;

    /**
     * @brief Get diagnostic record by index (1-based)
     */
    const DiagnosticRecord* getDiagnostic(SQLSMALLINT rec_number) const;

    /**
     * @brief Set error (helper for common case)
     */
    void setError(const std::string& sqlstate, SQLINTEGER native_error,
                  const std::string& message);

    /**
     * @brief Get last return code
     */
    SQLRETURN getReturnCode() const { return return_code_; }

    /**
     * @brief Set return code
     */
    void setReturnCode(SQLRETURN code) { return_code_ = code; }

protected:
    std::vector<DiagnosticRecord> diagnostics_;
    mutable std::mutex diagnostics_mutex_;
    SQLRETURN return_code_{SQL_SUCCESS};
};

// =============================================================================
// Environment Handle
// =============================================================================

/**
 * @brief ODBC Environment Handle
 *
 * Manages global driver state including ODBC version and connection pooling.
 */
class OdbcEnvironment : public OdbcHandle {
public:
    OdbcEnvironment();
    ~OdbcEnvironment() override;

    HandleType getType() const override { return HandleType::ENVIRONMENT; }

    // =========================================================================
    // Attribute Management
    // =========================================================================

    /**
     * @brief Set environment attribute
     */
    SQLRETURN setAttribute(SQLINTEGER attribute, SQLPOINTER value, SQLINTEGER string_length);

    /**
     * @brief Get environment attribute
     */
    SQLRETURN getAttribute(SQLINTEGER attribute, SQLPOINTER value,
                           SQLINTEGER buffer_length, SQLINTEGER* string_length);

    // =========================================================================
    // Connection Management
    // =========================================================================

    /**
     * @brief Create a new connection
     */
    OdbcConnection* createConnection();

    /**
     * @brief Remove a connection
     */
    void removeConnection(OdbcConnection* conn);

    /**
     * @brief Get connection count
     */
    size_t getConnectionCount() const;

    /**
     * @brief End transactions for all connected child connections
     */
    SQLRETURN endTransaction(SQLSMALLINT completion_type);

    // =========================================================================
    // Configuration
    // =========================================================================

    SQLUINTEGER getOdbcVersion() const { return odbc_version_; }
    SQLUINTEGER getConnectionPooling() const { return connection_pooling_; }
    void setConnectionPooling(SQLUINTEGER value) { connection_pooling_ = value; }
    SQLUINTEGER getCpMatch() const { return cp_match_; }
    bool getOutputNts() const { return output_nts_; }
    ScratchBird::ODBC::KeepaliveManager& keepaliveManager() { return keepalive_manager_; }
    ScratchBird::ODBC::LeakDetector& leakDetector() { return leak_detector_; }

private:
    SQLUINTEGER odbc_version_{SQL_OV_ODBC3_80};
    SQLUINTEGER connection_pooling_{SQL_CP_OFF};
    SQLUINTEGER cp_match_{SQL_CP_STRICT_MATCH};
    bool output_nts_{true};

    std::vector<std::unique_ptr<OdbcConnection>> connections_;
    mutable std::mutex connections_mutex_;
    ScratchBird::ODBC::KeepaliveManager keepalive_manager_;
    ScratchBird::ODBC::LeakDetector leak_detector_;
};

// =============================================================================
// Connection Handle
// =============================================================================

/**
 * @brief ODBC Connection Handle
 *
 * Manages a connection to a ScratchBird server.
 */
class OdbcConnection : public OdbcHandle {
public:
    explicit OdbcConnection(OdbcEnvironment* env);
    ~OdbcConnection() override;

    HandleType getType() const override { return HandleType::CONNECTION; }

    // =========================================================================
    // Connection Lifecycle
    // =========================================================================

    /**
     * @brief Connect using DSN
     */
    SQLRETURN connect(const SQLCHAR* dsn, SQLSMALLINT dsn_len,
                      const SQLCHAR* user, SQLSMALLINT user_len,
                      const SQLCHAR* password, SQLSMALLINT password_len);

    /**
     * @brief Connect using connection string
     */
    SQLRETURN driverConnect(HWND window_handle,
                            const SQLCHAR* conn_str, SQLSMALLINT conn_str_len,
                            SQLCHAR* out_conn_str, SQLSMALLINT out_buffer_len,
                            SQLSMALLINT* out_conn_str_len,
                            SQLUSMALLINT driver_completion);

    /**
     * @brief Browse connect (iterative)
     */
    SQLRETURN browseConnect(const SQLCHAR* in_conn_str, SQLSMALLINT in_conn_str_len,
                            SQLCHAR* out_conn_str, SQLSMALLINT out_buffer_len,
                            SQLSMALLINT* out_conn_str_len);

    /**
     * @brief Disconnect from server
     */
    SQLRETURN disconnect();

    /**
     * @brief Check if connected
     */
    bool isConnected() const { return connected_; }

    // =========================================================================
    // Attribute Management
    // =========================================================================

    /**
     * @brief Set connection attribute
     */
    SQLRETURN setAttribute(SQLINTEGER attribute, SQLPOINTER value, SQLINTEGER string_length);

    /**
     * @brief Get connection attribute
     */
    SQLRETURN getAttribute(SQLINTEGER attribute, SQLPOINTER value,
                           SQLINTEGER buffer_length, SQLINTEGER* string_length);

    // =========================================================================
    // Transaction Control
    // =========================================================================

    /**
     * @brief End transaction (commit or rollback)
     */
    SQLRETURN endTransaction(SQLSMALLINT completion_type);

    /**
     * @brief Begin explicit transaction
     */
    SQLRETURN beginTransaction();

    // =========================================================================
    // Information Retrieval
    // =========================================================================

    /**
     * @brief Get driver/database info
     */
    SQLRETURN getInfo(SQLUSMALLINT info_type, SQLPOINTER info_value,
                      SQLSMALLINT buffer_length, SQLSMALLINT* string_length);

    /**
     * @brief Get supported functions
     */
    SQLRETURN getFunctions(SQLUSMALLINT function_id, SQLUSMALLINT* supported);

    /**
     * @brief Get type info
     */
    SQLRETURN getTypeInfo(SQLSMALLINT data_type, OdbcStatement* stmt);

    // =========================================================================
    // Statement Management
    // =========================================================================

    /**
     * @brief Create a new statement
     */
    OdbcStatement* createStatement();

    /**
     * @brief Remove a statement
     */
    void removeStatement(OdbcStatement* stmt);

    /**
     * @brief Get statement count
     */
    size_t getStatementCount() const;

    // =========================================================================
    // Native Client Operations
    // =========================================================================

    /**
     * @brief Execute SQL on server
     */
    SQLRETURN executeSQL(const std::string& sql, std::vector<std::vector<std::string>>& results,
                         std::vector<ColumnMetadata>& columns, SQLLEN& rows_affected);

    /**
     * @brief Cancel the currently running statement
     */
    SQLRETURN cancel();

    /**
     * @brief Prepare statement on server
     */
    SQLRETURN prepareSQL(const std::string& sql, uint64_t& stmt_id,
                         std::vector<ColumnMetadata>& param_metadata);

    /**
     * @brief Execute prepared statement
     */
    SQLRETURN executePrepared(uint64_t stmt_id,
                              const std::vector<ParameterLiteral>& params,
                              std::vector<std::vector<std::string>>& results,
                              std::vector<ColumnMetadata>& columns,
                              SQLLEN& rows_affected);

    /**
     * @brief Build SQL text for a prepared statement with parameters applied
     */
    SQLRETURN buildPreparedSQL(uint64_t stmt_id,
                               const std::vector<ParameterLiteral>& params,
                               std::string& out_sql);

    // =========================================================================
    // Accessors
    // =========================================================================

    OdbcEnvironment* getEnvironment() { return env_; }
    const ConnectionParams& getParams() const { return params_; }
    const std::string& getCurrentDatabase() const { return current_database_; }
    const std::string& getCurrentUser() const { return current_user_; }
    const std::string& getCurrentSchema() const { return current_schema_; }
    bool getMetadataId() const { return metadata_id_; }

private:
    bool allowRequest();
    void recordSuccess();
    void recordFailure();
    void registerResilience();
    void unregisterResilience();

    SQLRETURN parseConnectionString(const std::string& conn_str);
    SQLRETURN applyDsnConfig(const std::string& dsn_name);
    SQLRETURN establishConnection();
    SQLRETURN applyAutocommitSetting();
    SQLRETURN applyIsolationSetting();
    std::string buildConnectionString() const;

    OdbcEnvironment* env_;
    int socket_fd_{-1};  // Socket file descriptor (or INVALID_SOCKET on Windows)

    ConnectionParams params_;
    bool connected_{false};
    std::string current_database_;
    std::string current_user_;
    std::string current_schema_;

    // Connection attributes
    SQLUINTEGER access_mode_{SQL_MODE_READ_WRITE};
    SQLUINTEGER auto_commit_{SQL_AUTOCOMMIT_ON};
    SQLUINTEGER login_timeout_{30};
    SQLUINTEGER connection_timeout_{0};
    SQLUINTEGER txn_isolation_{SQL_TXN_READ_COMMITTED};
    SQLUINTEGER packet_size_{8192};
    bool connection_dead_{false};
    // Tracks the ODBC wrapper's explicit transaction mode. The native bridge
    // can still own an active fresh MGA boundary while this remains false,
    // so live recovery truth comes from the bridge/runtime rather than this
    // wrapper-local flag alone.
    bool in_transaction_{false};
    bool metadata_id_{false};

    std::vector<std::unique_ptr<OdbcStatement>> statements_;
    mutable std::mutex statements_mutex_;

    std::unique_ptr<OdbcClientBridge> client_bridge_;

    // Server-side state
    uint64_t server_session_id_{0};
    uint32_t server_protocol_version_{0};

    std::unordered_map<uint64_t, std::string> prepared_sql_;
    std::string connection_id_;
    ScratchBird::ODBC::CircuitBreaker circuit_breaker_;
    ScratchBird::ODBC::KeepaliveTracker* keepalive_tracker_{nullptr};
    std::unique_ptr<ScratchBird::ODBC::LeakDetectionGuard> leak_guard_;
};

// =============================================================================
// Statement Handle
// =============================================================================

/**
 * @brief ODBC Statement Handle
 *
 * Manages SQL statement execution, parameter binding, and result fetching.
 */
class OdbcStatement : public OdbcHandle {
public:
    explicit OdbcStatement(OdbcConnection* conn);
    ~OdbcStatement() override;

    HandleType getType() const override { return HandleType::STATEMENT; }

    // =========================================================================
    // Statement Execution
    // =========================================================================

    /**
     * @brief Prepare SQL statement
     */
    SQLRETURN prepare(const SQLCHAR* sql, SQLINTEGER sql_len);

    /**
     * @brief Execute prepared statement
     */
    SQLRETURN execute();

    /**
     * @brief Execute SQL directly
     */
    SQLRETURN execDirect(const SQLCHAR* sql, SQLINTEGER sql_len);

    /**
     * @brief Cancel execution
     */
    SQLRETURN cancel();

    /**
     * @brief Close cursor
     */
    SQLRETURN closeCursor();

    /**
     * @brief Free statement resources
     */
    SQLRETURN freeStmt(SQLUSMALLINT option);

    // =========================================================================
    // Parameter Binding
    // =========================================================================

    /**
     * @brief Bind parameter
     */
    SQLRETURN bindParameter(SQLUSMALLINT parameter_number,
                            SQLSMALLINT input_output_type,
                            SQLSMALLINT value_type,
                            SQLSMALLINT parameter_type,
                            SQLULEN column_size,
                            SQLSMALLINT decimal_digits,
                            SQLPOINTER parameter_value,
                            SQLLEN buffer_length,
                            SQLLEN* str_len_or_ind);

    /**
     * @brief Get number of parameters
     */
    SQLRETURN numParams(SQLSMALLINT* param_count);

    /**
     * @brief Describe parameter
     */
    SQLRETURN describeParam(SQLUSMALLINT parameter_number,
                            SQLSMALLINT* data_type,
                            SQLULEN* parameter_size,
                            SQLSMALLINT* decimal_digits,
                            SQLSMALLINT* nullable);

    // =========================================================================
    // Column Binding
    // =========================================================================

    /**
     * @brief Bind column for retrieval
     */
    SQLRETURN bindCol(SQLUSMALLINT column_number,
                      SQLSMALLINT target_type,
                      SQLPOINTER target_value,
                      SQLLEN buffer_length,
                      SQLLEN* str_len_or_ind);

    /**
     * @brief Get number of result columns
     */
    SQLRETURN numResultCols(SQLSMALLINT* column_count);

    /**
     * @brief Describe column
     */
    SQLRETURN describeCol(SQLUSMALLINT column_number,
                          SQLCHAR* column_name,
                          SQLSMALLINT buffer_length,
                          SQLSMALLINT* name_length,
                          SQLSMALLINT* data_type,
                          SQLULEN* column_size,
                          SQLSMALLINT* decimal_digits,
                          SQLSMALLINT* nullable);

    /**
     * @brief Get column attribute
     */
    SQLRETURN colAttribute(SQLUSMALLINT column_number,
                           SQLUSMALLINT field_identifier,
                           SQLPOINTER char_attr,
                           SQLSMALLINT buffer_length,
                           SQLSMALLINT* string_length,
                           SQLLEN* numeric_attr);

    /**
     * @brief Set cursor name used by positioned operations.
     */
    SQLRETURN setCursorName(const SQLCHAR* cursor_name, SQLSMALLINT name_length);

    /**
     * @brief Get current cursor name.
     */
    SQLRETURN getCursorName(SQLCHAR* cursor_name, SQLSMALLINT buffer_length,
                            SQLSMALLINT* name_length);

    // =========================================================================
    // Data Retrieval
    // =========================================================================

    /**
     * @brief Fetch next row
     */
    SQLRETURN fetch();

    /**
     * @brief Fetch with scroll
     */
    SQLRETURN fetchScroll(SQLSMALLINT fetch_orientation, SQLLEN fetch_offset);

    /**
     * @brief Get data for unbound column
     */
    SQLRETURN getData(SQLUSMALLINT column_number,
                      SQLSMALLINT target_type,
                      SQLPOINTER target_value,
                      SQLLEN buffer_length,
                      SQLLEN* str_len_or_ind);

    /**
     * @brief Return token for parameter requiring streamed data
     */
    SQLRETURN paramData(SQLPOINTER* token);

    /**
     * @brief Provide chunks for parameters bound SQL_DATA_AT_EXEC
     */
    SQLRETURN putData(SQLPOINTER data, SQLLEN len);

    /**
     * @brief Get row count
     */
    SQLRETURN rowCount(SQLLEN* row_count);

    /**
     * @brief Check for more results
     */
    SQLRETURN moreResults();

    // =========================================================================
    // Positioned Operations
    // =========================================================================

    /**
     * @brief Set cursor position
     */
    SQLRETURN setPos(SQLSETPOSIROW row_number, SQLUSMALLINT operation, SQLUSMALLINT lock_type);

    /**
     * @brief Bulk operations
     */
    SQLRETURN bulkOperations(SQLSMALLINT operation);

    // =========================================================================
    // Catalog Functions
    // =========================================================================

    SQLRETURN tables(const SQLCHAR* catalog, SQLSMALLINT catalog_len,
                     const SQLCHAR* schema, SQLSMALLINT schema_len,
                     const SQLCHAR* table, SQLSMALLINT table_len,
                     const SQLCHAR* table_type, SQLSMALLINT table_type_len);

    SQLRETURN columns(const SQLCHAR* catalog, SQLSMALLINT catalog_len,
                      const SQLCHAR* schema, SQLSMALLINT schema_len,
                      const SQLCHAR* table, SQLSMALLINT table_len,
                      const SQLCHAR* column, SQLSMALLINT column_len);

    SQLRETURN primaryKeys(const SQLCHAR* catalog, SQLSMALLINT catalog_len,
                          const SQLCHAR* schema, SQLSMALLINT schema_len,
                          const SQLCHAR* table, SQLSMALLINT table_len);

    SQLRETURN foreignKeys(const SQLCHAR* pk_catalog, SQLSMALLINT pk_catalog_len,
                          const SQLCHAR* pk_schema, SQLSMALLINT pk_schema_len,
                          const SQLCHAR* pk_table, SQLSMALLINT pk_table_len,
                          const SQLCHAR* fk_catalog, SQLSMALLINT fk_catalog_len,
                          const SQLCHAR* fk_schema, SQLSMALLINT fk_schema_len,
                          const SQLCHAR* fk_table, SQLSMALLINT fk_table_len);

    SQLRETURN statistics(const SQLCHAR* catalog, SQLSMALLINT catalog_len,
                         const SQLCHAR* schema, SQLSMALLINT schema_len,
                         const SQLCHAR* table, SQLSMALLINT table_len,
                         SQLUSMALLINT unique, SQLUSMALLINT reserved);

    SQLRETURN specialColumns(SQLUSMALLINT identifier_type,
                             const SQLCHAR* catalog, SQLSMALLINT catalog_len,
                             const SQLCHAR* schema, SQLSMALLINT schema_len,
                             const SQLCHAR* table, SQLSMALLINT table_len,
                             SQLUSMALLINT scope, SQLUSMALLINT nullable);

    SQLRETURN procedures(const SQLCHAR* catalog, SQLSMALLINT catalog_len,
                         const SQLCHAR* schema, SQLSMALLINT schema_len,
                         const SQLCHAR* proc, SQLSMALLINT proc_len);

    SQLRETURN procedureColumns(const SQLCHAR* catalog, SQLSMALLINT catalog_len,
                               const SQLCHAR* schema, SQLSMALLINT schema_len,
                               const SQLCHAR* proc, SQLSMALLINT proc_len,
                               const SQLCHAR* column, SQLSMALLINT column_len);

    SQLRETURN tablePrivileges(const SQLCHAR* catalog, SQLSMALLINT catalog_len,
                              const SQLCHAR* schema, SQLSMALLINT schema_len,
                              const SQLCHAR* table, SQLSMALLINT table_len);

    SQLRETURN columnPrivileges(const SQLCHAR* catalog, SQLSMALLINT catalog_len,
                               const SQLCHAR* schema, SQLSMALLINT schema_len,
                               const SQLCHAR* table, SQLSMALLINT table_len,
                               const SQLCHAR* column, SQLSMALLINT column_len);

    // =========================================================================
    // Attribute Management
    // =========================================================================

    /**
     * @brief Set statement attribute
     */
    SQLRETURN setAttribute(SQLINTEGER attribute, SQLPOINTER value, SQLINTEGER string_length);

    /**
     * @brief Get statement attribute
     */
    SQLRETURN getAttribute(SQLINTEGER attribute, SQLPOINTER value,
                           SQLINTEGER buffer_length, SQLINTEGER* string_length);

    /**
     * @brief Set result set for catalog/metadata queries
     */
    void setCatalogResult(std::vector<ColumnMetadata> columns,
                          std::vector<std::vector<std::string>> rows);

    // =========================================================================
    // Accessors
    // =========================================================================

    OdbcConnection* getConnection() { return conn_; }
    bool isPrepared() const { return prepared_; }
    bool hasResults() const { return has_results_; }
    SQLLEN getRowCount() const { return row_count_; }
    OdbcDescriptor* getAppParamDescriptor() const { return app_param_desc_; }
    OdbcDescriptor* getImpParamDescriptor() const { return ipd_desc_; }
    OdbcDescriptor* getAppRowDescriptor() const { return app_row_desc_; }
    OdbcDescriptor* getImpRowDescriptor() const { return ird_desc_; }

private:
    struct ResultSet {
        std::vector<ColumnMetadata> columns;
        std::vector<std::vector<std::string>> rows;
        SQLLEN row_count{0};
    };

    SQLRETURN bindResultData();
    SQLRETURN convertAndStore(size_t col_index, const std::string& value);
    void clearGetDataState();
    void clearPutDataState();
    bool isDataAtExecIndicator(SQLLEN indicator) const;
    SQLRETURN validateOrInitDataAtExecState();
    SQLRETURN validateOrInitDataAtExecStateForRow(SQLULEN row_offset);
    SQLUSMALLINT getCurrentDataAtExecParameter() const;
    SQLPOINTER putDataTokenToPointer(SQLUSMALLINT parameter_number) const;
    SQLUSMALLINT pointerToPutDataToken(SQLPOINTER token) const;
    uint64_t putDataStreamKey(SQLUSMALLINT parameter_number, SQLULEN row_offset) const;
    const SQLLEN* indicatorForRow(const ParameterBinding& binding, SQLULEN row_offset) const;
    SQLRETURN buildParameterData(std::vector<ParameterLiteral>& literals, SQLULEN row_offset);
    std::vector<ParameterLiteral> buildParameterData();
    SQLRETURN executeSqlStatements(const std::string& sql);
    SQLRETURN getDataInternal(SQLUSMALLINT column_number,
                              SQLSMALLINT target_type,
                              SQLPOINTER target_value,
                              SQLLEN buffer_length,
                              SQLLEN* str_len_or_ind,
                              bool stream_chunks);
    void applyResultSet(size_t index);
    void resetResults();

    OdbcConnection* conn_;

    // Statement state
    std::string sql_;
    bool prepared_{false};
    bool executed_{false};
    bool has_results_{false};
    uint64_t server_stmt_id_{0};

    // Parameter bindings
    std::unordered_map<SQLUSMALLINT, ParameterBinding> param_bindings_;
    SQLULEN paramset_size_{1};
    SQLULEN* params_processed_ptr_{nullptr};
    SQLUSMALLINT* param_status_ptr_{nullptr};
    SQLLEN param_bind_offset_{0};
    SQLULEN param_bind_type_{0};  // SQL_PARAM_BIND_BY_COLUMN

    // Column bindings
    std::unordered_map<SQLUSMALLINT, ColumnBinding> col_bindings_;
    ColumnBinding bookmark_binding_{};
    bool bookmark_bound_{false};
    SQLULEN row_array_size_{1};
    SQLULEN* rows_fetched_ptr_{nullptr};
    SQLUSMALLINT* row_status_ptr_{nullptr};
    SQLLEN row_bind_offset_{0};
    SQLULEN row_bind_type_{0};  // SQL_BIND_BY_COLUMN
    BOOKMARK* fetch_bookmark_ptr_{nullptr};

    // Statement descriptor handles
    std::unique_ptr<OdbcDescriptor> owned_app_param_desc_;
    std::unique_ptr<OdbcDescriptor> owned_imp_param_desc_;
    std::unique_ptr<OdbcDescriptor> owned_app_row_desc_;
    std::unique_ptr<OdbcDescriptor> owned_ird_desc_;
    OdbcDescriptor* app_param_desc_{nullptr};
    OdbcDescriptor* ipd_desc_{nullptr};
    OdbcDescriptor* app_row_desc_{nullptr};
    OdbcDescriptor* ird_desc_{nullptr};

    // Result set
    std::vector<ColumnMetadata> columns_;
    std::vector<std::vector<std::string>> rows_;
    size_t current_row_{0};
    SQLLEN row_count_{-1};
    std::vector<ResultSet> result_sets_;
    size_t current_result_index_{0};

    // Cursor attributes
    SQLULEN cursor_type_{SQL_CURSOR_FORWARD_ONLY};
    SQLULEN concurrency_{SQL_CONCUR_READ_ONLY};
    SQLULEN cursor_scrollable_{0};  // SQL_NONSCROLLABLE
    SQLULEN cursor_sensitivity_{0};  // SQL_UNSPECIFIED

    // Query attributes
    SQLULEN query_timeout_{0};
    SQLULEN max_rows_{0};
    SQLULEN max_length_{0};
    bool noscan_{false};
    bool use_bookmarks_{false};
    bool retrieve_data_{true};
    std::string cursor_name_;

    // SQLGetData streaming state for long-data chunked retrieval
    struct GetDataStreamState {
        std::string value;
        size_t offset{0};
    };
    std::unordered_map<SQLUSMALLINT, GetDataStreamState> get_data_stream_;

    // Parameter streaming state for SQL_DATA_AT_EXEC / SQLPutData.
    struct PutDataStreamState {
        std::string value;
        SQLLEN expected_length{0};
        bool expected_length_known{false};
        bool complete{false};
        bool truncated{false};
    };
    std::unordered_map<uint64_t, PutDataStreamState> put_data_stream_;
    std::vector<SQLUSMALLINT> data_at_exec_params_;
    size_t data_at_exec_index_{0};
    bool data_at_exec_active_{false};
    SQLULEN data_at_exec_row_offset_{0};
};

// =============================================================================
// Descriptor Handle
// =============================================================================

/**
 * @brief ODBC Descriptor Handle
 *
 * Manages application and implementation descriptors for parameters and rows.
 */
class OdbcDescriptor : public OdbcHandle {
public:
    enum class DescriptorType {
        APD,  // Application Parameter Descriptor
        IPD,  // Implementation Parameter Descriptor
        ARD,  // Application Row Descriptor
        IRD   // Implementation Row Descriptor
    };

    OdbcDescriptor(OdbcConnection* conn, DescriptorType type, bool implicit);
    ~OdbcDescriptor() override;

    HandleType getType() const override { return HandleType::DESCRIPTOR; }

    // =========================================================================
    // Field Management
    // =========================================================================

    /**
     * @brief Set descriptor field
     */
    SQLRETURN setField(SQLSMALLINT rec_number, SQLSMALLINT field_identifier,
                       SQLPOINTER value, SQLINTEGER buffer_length);

    /**
     * @brief Get descriptor field
     */
    SQLRETURN getField(SQLSMALLINT rec_number, SQLSMALLINT field_identifier,
                       SQLPOINTER value, SQLINTEGER buffer_length, SQLINTEGER* string_length);

    /**
     * @brief Set descriptor record
     */
    SQLRETURN setRec(SQLSMALLINT rec_number, SQLSMALLINT type, SQLSMALLINT sub_type,
                     SQLLEN length, SQLSMALLINT precision, SQLSMALLINT scale,
                     SQLPOINTER data, SQLLEN* string_length, SQLLEN* indicator);

    /**
     * @brief Get descriptor record
     */
    SQLRETURN getRec(SQLSMALLINT rec_number, SQLCHAR* name, SQLSMALLINT buffer_length,
                     SQLSMALLINT* string_length, SQLSMALLINT* type, SQLSMALLINT* sub_type,
                     SQLLEN* length, SQLSMALLINT* precision, SQLSMALLINT* scale,
                     SQLSMALLINT* nullable);

    /**
     * @brief Copy descriptor
     */
    SQLRETURN copyDesc(OdbcDescriptor* target);

    // =========================================================================
    // Header Fields
    // =========================================================================

    SQLSMALLINT getCount() const { return count_; }
    void setCount(SQLSMALLINT count) { count_ = count; }
    SQLULEN getArraySize() const { return array_size_; }
    void setArraySize(SQLULEN size) { array_size_ = size; }

    // =========================================================================
    // Accessors
    // =========================================================================

    /**
     * @brief Clear descriptor records and header count
     */
    void resetDescriptor();

    OdbcConnection* getConnection() { return conn_; }
    DescriptorType getDescriptorType() const { return desc_type_; }
    bool isImplicit() const { return implicit_; }

private:
    struct DescriptorRecord {
        SQLSMALLINT type{SQL_UNKNOWN_TYPE};
        SQLSMALLINT concise_type{SQL_UNKNOWN_TYPE};
        SQLSMALLINT datetime_interval_code{0};
        SQLSMALLINT datetime_interval_precision{0};
        SQLSMALLINT maximum_scale{0};
        SQLSMALLINT minimum_scale{0};
        SQLSMALLINT nullable{SQL_NULLABLE_UNKNOWN};
        SQLSMALLINT precision{0};
        SQLSMALLINT scale{0};
        SQLSMALLINT unnamed{1};  // SQL_UNNAMED
        SQLSMALLINT unsigned_{0};
        SQLSMALLINT fixed_prec_scale{0};
        SQLSMALLINT auto_unique_value{0};
        SQLSMALLINT case_sensitive{1};
        SQLSMALLINT searchable{2};  // SQL_SEARCHABLE
        SQLSMALLINT updatable{0};
        SQLSMALLINT num_prec_radix{10};
        SQLLEN length{0};
        SQLLEN octet_length{0};
        SQLLEN display_size{0};
        std::string name;
        std::string base_column_name;
        std::string base_table_name;
        std::string catalog_name;
        std::string label;
        std::string literal_prefix;
        std::string literal_suffix;
        std::string local_type_name;
        std::string schema_name;
        std::string table_name;
        std::string type_name;
        SQLPOINTER data_ptr{nullptr};
        SQLLEN* indicator_ptr{nullptr};
        SQLLEN* octet_length_ptr{nullptr};
    };

    OdbcConnection* conn_;
    DescriptorType desc_type_;
    bool implicit_;

    // Header fields
    SQLSMALLINT count_{0};
    SQLULEN array_size_{1};
    SQLSMALLINT alloc_type_{SQL_DESC_ALLOC_AUTO};  // SQL_DESC_ALLOC_AUTO or SQL_DESC_ALLOC_USER
    SQLULEN* array_status_ptr_{nullptr};
    SQLLEN* bind_offset_ptr_{nullptr};
    SQLULEN bind_type_{0};
    SQLULEN* rows_processed_ptr_{nullptr};

    // Records (0 = bookmark, 1..n = columns/parameters)
    std::vector<DescriptorRecord> records_;
};

// =============================================================================
// Handle Validation Helpers
// =============================================================================

/**
 * @brief Validate handle type
 */
inline bool isValidHandle(SQLHANDLE handle, HandleType expected_type) {
    if (!handle) return false;
    auto* h = static_cast<OdbcHandle*>(handle);
    return h->getType() == expected_type;
}

/**
 * @brief Get handle as environment
 */
inline OdbcEnvironment* asEnvironment(SQLHENV handle) {
    if (!isValidHandle(handle, HandleType::ENVIRONMENT)) return nullptr;
    return static_cast<OdbcEnvironment*>(handle);
}

/**
 * @brief Get handle as connection
 */
inline OdbcConnection* asConnection(SQLHDBC handle) {
    if (!isValidHandle(handle, HandleType::CONNECTION)) return nullptr;
    return static_cast<OdbcConnection*>(handle);
}

/**
 * @brief Get handle as statement
 */
inline OdbcStatement* asStatement(SQLHSTMT handle) {
    if (!isValidHandle(handle, HandleType::STATEMENT)) return nullptr;
    return static_cast<OdbcStatement*>(handle);
}

/**
 * @brief Get handle as descriptor
 */
inline OdbcDescriptor* asDescriptor(SQLHDESC handle) {
    if (!isValidHandle(handle, HandleType::DESCRIPTOR)) return nullptr;
    return static_cast<OdbcDescriptor*>(handle);
}

}  // namespace odbc
}  // namespace scratchbird

#endif  // SCRATCHBIRD_ODBC_HANDLES_H
