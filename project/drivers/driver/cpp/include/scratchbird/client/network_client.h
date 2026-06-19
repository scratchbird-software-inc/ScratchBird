// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

/**
 * ScratchBird Network Client (libscratchbird)
 *
 * Alpha: Native protocol over network listener (parser bridge required).
 */

#include <cstdint>
#include <map>
#include <unordered_map>
#include <string>
#include <thread>
#include <vector>
#include <istream>
#include <ostream>

#include "scratchbird/core/status.h"
#include "scratchbird/core/error_context.h"
#include "scratchbird/network/network.h"
#include "scratchbird/protocol/sbwp_protocol.h"
#include "scratchbird/security/tls_config.h"

namespace scratchbird {
namespace client {

namespace protocol = scratchbird::protocol::sbwp;

struct NetworkClientConfig {
    // embedded | inet_listener | local_ipc | managed
    std::string transport_mode{"inet_listener"};
    std::string host{"127.0.0.1"};
    uint16_t port{network::DEFAULT_NATIVE_PORT};
    std::string ipc_method{"auto"};
    std::string ipc_path;

    std::string protocol{"native"};
    std::string front_door_mode{"direct"};  // direct | manager_proxy
    std::string database;
    std::string username;
    std::string password;
    std::string role;
    std::string schema;
    std::string application_name{"scratchbird_odbc"};

    // manager_proxy mode options
    std::string manager_auth_token;
    std::string manager_username;
    std::string manager_database;
    std::string manager_connection_profile{"SBsql"};
    std::string manager_client_intent{"SBsql"};
    uint16_t manager_client_flags{0};
    bool manager_auth_fast_path{true};

    uint32_t connect_timeout_ms{network::DEFAULT_CONNECT_TIMEOUT_MS};
    uint32_t read_timeout_ms{network::DEFAULT_READ_TIMEOUT_MS};
    uint32_t write_timeout_ms{network::DEFAULT_WRITE_TIMEOUT_MS};
    uint32_t copy_window_bytes{65536};
    uint32_t copy_chunk_bytes{16384};
    bool enable_copy_streaming{false};

    network::SSLMode ssl_mode{network::SSLMode::REQUIRE};
    std::string ssl_cert;
    std::string ssl_key;
    std::string ssl_root_cert;

    uint16_t connect_client_flags{0x0100};
    protocol::AuthMethod auth_method{protocol::AuthMethod::ScramSha256};
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
    bool allow_password_fallback{false};
    bool binary_transfer{true};
    bool enable_compression{false};
    bool autocommit{true};
};

struct AuthMethodSurface {
    std::string wire_method;
    std::string plugin_method_id;
    bool executable_locally{false};
    bool broker_required{false};
};

struct AuthProbeResult {
    bool reachable{false};
    std::string ingress_mode;
    std::string resolved_host;
    uint16_t resolved_port{0};
    std::vector<AuthMethodSurface> admitted_methods;
    std::string required_method;
    std::string required_plugin_method_id;
    bool additional_continuation_possible{false};
};

struct ResolvedAuthContext {
    std::string ingress_mode{"direct"};
    std::string resolved_auth_method;
    std::string resolved_auth_plugin_id;
    // Manager authentication is transport admission to the manager proxy only.
    // The attached server session is established by the subsequent SBsql
    // server/parser authentication handshake.
    bool manager_authenticated{false};
    bool attached{false};
};

struct NetworkColumn {
    std::string name;
    uint32_t type_oid{0};
    int32_t type_modifier{0};
    uint8_t format{protocol::kFormatBinary};
    bool nullable{true};
};

struct NetworkResultSet {
    std::vector<NetworkColumn> columns;
    std::vector<std::vector<protocol::ColumnValue>> rows;
    std::vector<uint8_t> copy_data;
    std::vector<uint32_t> copy_column_formats;
    int64_t rows_affected{0};
    std::string command_tag;
    uint8_t copy_format{protocol::kFormatText};
    bool copy_active{false};
};

struct NetworkTransactionFinality {
    protocol::TxnFinalityState state{protocol::TxnFinalityState::Unknown};
    uint16_t flags{0};
    std::array<uint8_t, 16> idempotency_key{};
    std::array<uint8_t, 16> finality_token{};
    uint64_t request_fingerprint{0};
    uint64_t original_txn_id{0};
    uint64_t replacement_txn_id{0};
    std::string diagnostic_code;
    std::string detail;

    bool engineFinalityKnown() const {
        return (flags & protocol::kTxnFinalityFlagEngineKnown) != 0;
    }
    bool retryAllowed() const {
        return (flags & protocol::kTxnFinalityFlagRetryAllowed) != 0;
    }
    bool retryRefused() const {
        return (flags & protocol::kTxnFinalityFlagRetryRefused) != 0;
    }
    bool sideEffectRetryRefused() const {
        return (flags & protocol::kTxnFinalityFlagSideEffectRetryRefused) != 0;
    }
    bool sameIdempotencyKeyReplayable() const {
        return (flags & protocol::kTxnFinalityFlagSameIdempotencyKeyReplayable) != 0;
    }
    bool postInventorySecondaryFailure() const {
        return (flags & protocol::kTxnFinalityFlagPostInventorySecondaryFailure) != 0;
    }
};

class NetworkPreparedStatement {
public:
    NetworkPreparedStatement();
    ~NetworkPreparedStatement();

    NetworkPreparedStatement(NetworkPreparedStatement&& other) noexcept;
    NetworkPreparedStatement& operator=(NetworkPreparedStatement&& other) noexcept;
    NetworkPreparedStatement(const NetworkPreparedStatement&) = delete;
    NetworkPreparedStatement& operator=(const NetworkPreparedStatement&) = delete;

    size_t getParameterCount() const;
    bool isValid() const;
    void clearParameters();

    void setNull(size_t index);
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
    void setBinary(size_t index, const uint8_t* data, size_t length, uint32_t type_oid, bool length_prefixed);
    void setTimestamp(size_t index, int64_t microseconds);
    void setDate(size_t index, int32_t days);
    void setTime(size_t index, int64_t microseconds);
    void setUUID(size_t index, const std::vector<uint8_t>& value);
    void setUUID(size_t index, const std::string& value);
    void setNull(size_t index, uint32_t type_oid);

private:
    friend class NetworkClient;
    std::string sql_;
    std::string statement_name_;
    size_t param_count_{0};
    std::vector<protocol::ParamValue> params_;
    std::vector<uint32_t> param_type_oids_;
    std::vector<uint32_t> prepared_type_oids_;
    std::array<uint8_t, 16> owner_session_id_{};
    uintptr_t owner_client_token_{0};
    bool valid_{false};
};

class NetworkClient {
public:
    struct TransactionOptions {
        uint16_t flags{0};
        uint8_t conflict_action{0};
        uint8_t autocommit_mode{0};
        uint8_t isolation_level{0};
        uint8_t read_committed_mode{0};
        uint8_t access_mode{0};
        uint8_t deferrable{0};
        uint8_t wait_mode{0};
        uint32_t timeout_ms{0};
    };

    NetworkClient();
    ~NetworkClient();

    core::Status connect(const NetworkClientConfig& config,
                         core::ErrorContext* ctx = nullptr);
    core::Status probeAuthSurface(const NetworkClientConfig& config,
                                  AuthProbeResult& result,
                                  core::ErrorContext* ctx = nullptr);
    void disconnect();

    bool isConnected() const;
    bool inTransaction() const { return in_transaction_; }
    uint64_t currentTransactionId() const { return current_txn_id_; }
    std::string parameterStatus(const std::string& name) const {
        auto it = parameter_status_.find(name);
        return it == parameter_status_.end() ? std::string{} : it->second;
    }
    const std::string& lastError() const { return last_error_; }
    ResolvedAuthContext getResolvedAuthContext() const { return resolved_auth_context_; }
    void setAutocommit(bool enabled) { config_.autocommit = enabled; }
    bool getAutocommit() const { return config_.autocommit; }

    core::Status executeQuery(const std::string& sql,
                              NetworkResultSet& results,
                              core::ErrorContext* ctx = nullptr);
    core::Status prepare(const std::string& sql,
                         NetworkPreparedStatement& stmt,
                         core::ErrorContext* ctx = nullptr);
    core::Status executePrepared(NetworkPreparedStatement& stmt,
                                 NetworkResultSet& results,
                                 core::ErrorContext* ctx = nullptr);
    core::Status prepareServerStatement(const std::string& sql,
                                        uint32_t& stmt_id,
                                        core::ErrorContext* ctx = nullptr);
    core::Status executeServerStatement(uint32_t stmt_id,
                                        const std::vector<protocol::ParamValue>& params,
                                        NetworkResultSet& results,
                                        uint32_t max_rows,
                                        bool backward,
                                        bool* portal_suspended_out,
                                        core::ErrorContext* ctx = nullptr);
    core::Status closeServerStatement(uint32_t stmt_id,
                                      core::ErrorContext* ctx = nullptr);
    core::Status sendQueryCancel(core::ErrorContext* ctx = nullptr);
    core::Status subscribeNotifications(uint8_t subscribe_type,
                                        const std::string& channel,
                                        const std::string& filter,
                                        core::ErrorContext* ctx = nullptr);
    core::Status unsubscribeNotifications(const std::string& channel,
                                          core::ErrorContext* ctx = nullptr);

    core::Status streamControl(uint8_t control_type,
                               uint32_t window_size,
                               uint32_t timeout_ms,
                               core::ErrorContext* ctx = nullptr);

    core::Status attachCreate(const std::string& mode,
                              const std::string& db_name,
                              core::ErrorContext* ctx = nullptr);
    core::Status attachDetach(core::ErrorContext* ctx = nullptr);
    core::Status attachList(NetworkResultSet& results,
                            core::ErrorContext* ctx = nullptr);

    core::Status executeSblr(uint64_t sblr_hash,
                             const std::vector<uint8_t>& bytecode,
                             const std::vector<protocol::ParamValue>& params,
                             NetworkResultSet& results,
                             core::ErrorContext* ctx = nullptr);

    core::Status setOption(const std::string& name,
                           const std::string& value,
                           core::ErrorContext* ctx = nullptr);
    core::Status ping(core::ErrorContext* ctx = nullptr);

    struct QueryProgressSnapshot {
        uint64_t rows_processed = 0;
        uint64_t bytes_processed = 0;
        uint64_t updated_at_micros = 0;
    };

    void resetQueryProgress();
    QueryProgressSnapshot queryProgress() const { return query_progress_; }

    struct Notification {
        uint32_t process_id = 0;
        std::string channel;
        std::vector<uint8_t> payload;
        uint8_t change_type = 0;
        uint64_t row_id = 0;
    };

    void drainNotifications(std::vector<Notification>& out);

    bool takeLastQueryPlan(protocol::QueryPlan& out);
    bool takeLastSblrCompiled(protocol::SblrCompiled& out);

    core::Status beginTransaction(const TransactionOptions& options,
                                  core::ErrorContext* ctx = nullptr);
    core::Status beginTransaction(core::ErrorContext* ctx = nullptr);
    core::Status commit(core::ErrorContext* ctx = nullptr);
    const NetworkTransactionFinality& lastCommitFinality() const {
        return last_commit_finality_;
    }
    std::array<uint8_t, 16> lastCommitIdempotencyKey() const {
        return last_commit_finality_.idempotency_key;
    }
    core::Status queryLastCommitFinality(core::ErrorContext* ctx = nullptr);
    core::Status queryCommitFinality(const std::array<uint8_t, 16>& idempotency_key,
                                     const std::array<uint8_t, 16>& finality_token,
                                     NetworkTransactionFinality& finality,
                                     core::ErrorContext* ctx = nullptr);
    core::Status validateRetryAfterCommitUncertainty(
        const std::array<uint8_t, 16>& idempotency_key,
        bool statement_has_side_effects,
        bool caller_acknowledged_retry_boundary,
        core::ErrorContext* ctx = nullptr) const;
    core::Status rollback(core::ErrorContext* ctx = nullptr);
    core::Status savepoint(const std::string& name,
                           core::ErrorContext* ctx = nullptr);
    core::Status releaseSavepoint(const std::string& name,
                                  core::ErrorContext* ctx = nullptr);
    core::Status rollbackToSavepoint(const std::string& name,
                                     core::ErrorContext* ctx = nullptr);

    void setCopyInputStream(std::istream* in) { copy_input_stream_ = in; }
    void setCopyOutputStream(std::ostream* out) { copy_output_stream_ = out; }

private:
    core::Status openSocket(bool require_identity,
                            bool require_manager_token,
                            core::ErrorContext* ctx = nullptr);
    core::Status openLocalIpcBridge(core::ErrorContext* ctx = nullptr);
    core::Status openEmbeddedBridge(core::ErrorContext* ctx = nullptr);
    void disconnectSocketForReconnect();
    void joinLocalIpcBridge();
    void resetResolvedAuthContext();
    core::Status buildStartupParams(uint64_t& features_out,
                                    std::map<std::string, std::string>& params_out,
                                    core::ErrorContext* ctx = nullptr);
    core::Status probeDirectAuthSurface(AuthProbeResult& result,
                                        core::ErrorContext* ctx = nullptr);
    core::Status probeManagerAuthSurface(AuthProbeResult& result,
                                         core::ErrorContext* ctx = nullptr);
    core::Status sendManagerFrame(uint8_t type,
                                  const std::vector<uint8_t>& payload,
                                  core::ErrorContext* ctx = nullptr);
    core::Status receiveManagerFrame(uint8_t& type,
                                     std::vector<uint8_t>& payload,
                                     core::ErrorContext* ctx = nullptr);
    core::Status performManagerConnect(core::ErrorContext* ctx = nullptr);

    core::Status sendMessage(const protocol::ProtocolMessage& msg,
                             core::ErrorContext* ctx = nullptr);
    core::Status sendMessage(protocol::MessageType type,
                             const std::vector<uint8_t>& payload,
                             uint8_t flags,
                             bool force_zero,
                             uint32_t* sequence_out,
                             core::ErrorContext* ctx);
    core::Status receiveMessage(protocol::ProtocolMessage& msg,
                                core::ErrorContext* ctx = nullptr);
    core::Status readExactWithTimeout(void* buffer, size_t size,
                                      core::ErrorContext* ctx = nullptr);
    core::Status sendCopyInputStream(core::ErrorContext* ctx = nullptr);
    core::Status handleCopyOutResponseMessage(const protocol::ProtocolMessage& msg,
                                              NetworkResultSet& results,
                                              core::ErrorContext* ctx = nullptr);
    core::Status handleCopyBothResponseMessage(const protocol::ProtocolMessage& msg,
                                               NetworkResultSet& results,
                                               core::ErrorContext* ctx = nullptr);
    core::Status handleCopyDataMessage(const protocol::ProtocolMessage& msg,
                                       NetworkResultSet& results,
                                       core::ErrorContext* ctx = nullptr);
    core::Status handleCopyFailMessage(const protocol::ProtocolMessage& msg,
                                       core::ErrorContext* ctx = nullptr);
    core::Status handleAsyncMessage(const protocol::ProtocolMessage& msg,
                                    core::ErrorContext* ctx = nullptr);
    core::Status handleTxnStatusMessage(const protocol::ProtocolMessage& msg,
                                        core::ErrorContext* ctx = nullptr);
    core::Status handleErrorResponse(const protocol::ProtocolMessage& msg,
                                     core::ErrorContext* ctx = nullptr);
    core::Status drainUntilReady(std::string* command_tag,
                                 uint64_t* rows,
                                 uint64_t* last_id,
                                 core::ErrorContext* ctx = nullptr);
    core::Status finishAutocommitStatement(bool statement_succeeded,
                                           core::ErrorContext* ctx = nullptr);
    bool serverAutocommitRequested() const;
    core::Status readyAfterStatement(const std::vector<uint8_t>& payload,
                                     core::ErrorContext* ctx = nullptr);
    core::Status errorAfterStatement(const protocol::ProtocolMessage& msg,
                                     core::ErrorContext* ctx = nullptr);
    core::Status handshake(core::ErrorContext* ctx);

    NetworkClientConfig config_{};
    std::unique_ptr<network::NetworkInitGuard> network_guard_{};
    std::unique_ptr<network::Socket> socket_{};
    std::unique_ptr<security::TLSContext> tls_ctx_{};
    std::unique_ptr<security::TLSConnection> tls_conn_{};
    std::thread local_ipc_bridge_thread_;
    bool tls_active_{false};
    std::array<uint8_t, 16> session_id_{};
    QueryProgressSnapshot query_progress_{};
    std::vector<Notification> notifications_;
    std::map<std::string, std::string> parameter_status_;
    std::unique_ptr<protocol::QueryPlan> last_plan_;
    std::unique_ptr<protocol::SblrCompiled> last_sblr_;

    bool connected_{false};
    bool in_transaction_{false};
    bool explicit_transaction_active_{false};
    bool server_autocommit_requested_{false};
    uint64_t current_txn_id_{0};
    NetworkTransactionFinality last_commit_finality_{};
    bool last_commit_finality_present_{false};
    std::string last_error_;
    ResolvedAuthContext resolved_auth_context_{};
    uint32_t next_sequence_{0};
    uint32_t last_query_sequence_{0};
    std::unordered_map<uint32_t, NetworkPreparedStatement> prepared_statements_;

    std::istream* copy_input_stream_{nullptr};
    std::ostream* copy_output_stream_{nullptr};

    bool compression_enabled_{false};
};

void applyDriverDefaultsFromEnv(NetworkClientConfig& config);

} // namespace client
} // namespace scratchbird
