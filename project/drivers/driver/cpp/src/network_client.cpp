// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/**
 * ScratchBird Network Client (SBWP v1.1)
 */

#include "scratchbird/client/network_client.h"
#include "scratchbird/client/driver_config.h"
#include "scratchbird/core/sqlstate.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string_view>
#include <utility>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#if defined(SCRATCHBIRD_CLIENT_ENABLE_LOCAL_SBSQL_BRIDGE)
#include "wire/sbsql_test_wire.hpp"
#endif
#if defined(SCRATCHBIRD_CLIENT_ENABLE_LOCAL_SBSQL_BRIDGE) && \
    defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
#include "database_ownership.hpp"
#include "memory.hpp"
#endif

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace scratchbird {
namespace client {

namespace {
constexpr uint32_t kDefaultTimeoutMs = 30000;
constexpr uint32_t kCancelTypeStatement = 0;
constexpr uint8_t kDescribeStatement = 'S';
constexpr uint8_t kCloseStatement = 'S';
constexpr int64_t kMicrosPerSecond = 1000000LL;
constexpr int64_t kMicrosPerDay = 86400LL * kMicrosPerSecond;
constexpr int64_t kDaysFrom1970To2000 = 10957;

bool isZeroUuidBytes(const std::array<uint8_t, 16>& uuid) {
    return std::all_of(uuid.begin(), uuid.end(), [](uint8_t byte) {
        return byte == 0;
    });
}

bool uuidBytesEqual(const std::array<uint8_t, 16>& left,
                    const std::array<uint8_t, 16>& right) {
    return std::equal(left.begin(), left.end(), right.begin(), right.end());
}

std::array<uint8_t, 16> randomProtocolUuid() {
    std::array<uint8_t, 16> out{};
    if (RAND_bytes(out.data(), static_cast<int>(out.size())) != 1) {
        std::random_device rd;
        for (auto& byte : out) {
            byte = static_cast<uint8_t>(rd() & 0xffu);
        }
    }
    out[6] = static_cast<uint8_t>((out[6] & 0x0fu) | 0x70u);
    out[8] = static_cast<uint8_t>((out[8] & 0x3fu) | 0x80u);
    return out;
}

uint64_t fnv1a64(std::string_view text) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char ch : text) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ull;
    }
    return hash;
}

NetworkTransactionFinality toNetworkFinality(const protocol::TxnFinalityStatus& status) {
    NetworkTransactionFinality out;
    out.state = status.state;
    out.flags = status.flags;
    out.idempotency_key = status.idempotency_key;
    out.finality_token = status.finality_token;
    out.request_fingerprint = status.request_fingerprint;
    out.original_txn_id = status.original_txn_id;
    out.replacement_txn_id = status.replacement_txn_id;
    out.diagnostic_code = status.diagnostic_code;
    out.detail = status.detail;
    return out;
}

// manager MCP uses the SBDB frame header before switching to byte-proxy mode.
constexpr uint32_t kManagerProtocolMagic = 0x42444253;  // "SBDB"
constexpr uint16_t kManagerProtocolVersion = 0x0101;    // 1.1
constexpr size_t kManagerHeaderSize = 12;
constexpr uint32_t kManagerMaxPayloadSize = 16u * 1024u * 1024u;
constexpr uint16_t kMcpProtocolVersion = 0x0100;

constexpr uint8_t kMsgConnectResponse = 0x02;
constexpr uint8_t kMsgAuthChallenge = 0x12;
constexpr uint8_t kMsgAuthResponse = 0x11;
constexpr uint8_t kMsgStatusResponse = 0x64;
constexpr uint8_t kMsgMcpHello = 0x65;
constexpr uint8_t kMsgMcpAuthStart = 0x66;
constexpr uint8_t kMsgMcpAuthContinue = 0x67;
constexpr uint8_t kMsgMcpDbConnect = 0x69;
constexpr uint8_t kMcpAuthMethodToken = 4;

bool wireDebugEnabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("SCRATCHBIRD_DRIVER_WIRE_DEBUG");
        if (!value || value[0] == '\0') {
            return false;
        }
        std::string normalized(value);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        return normalized != "0" &&
               normalized != "FALSE" &&
               normalized != "NO" &&
               normalized != "OFF";
    }();
    return enabled;
}

bool driverPhaseTraceEnabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("SCRATCHBIRD_CPP_DRIVER_PHASE_TRACE_FILE");
        return value != nullptr && value[0] != '\0';
    }();
    return enabled;
}

int64_t driverPhaseNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const char* messageTypeName(protocol::MessageType type);

std::string driverTraceJsonEscape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

void writeDriverPhaseTrace(std::string_view event,
                           std::string_view phase,
                           int64_t elapsed_ns,
                           size_t bytes,
                           size_t count,
                           protocol::MessageType type,
                           uint32_t sequence,
                           bool tls_active) {
    const char* trace_path = std::getenv("SCRATCHBIRD_CPP_DRIVER_PHASE_TRACE_FILE");
    if (trace_path == nullptr || trace_path[0] == '\0') {
        return;
    }
    static std::mutex trace_mutex;
    std::lock_guard<std::mutex> guard(trace_mutex);
    std::ofstream out(trace_path, std::ios::app);
    if (!out) {
        return;
    }
    out << "{\"event\":\"" << driverTraceJsonEscape(event) << "\""
        << ",\"phase\":\"" << driverTraceJsonEscape(phase) << "\""
        << ",\"elapsed_us\":" << (elapsed_ns / 1000)
        << ",\"bytes\":" << bytes
        << ",\"count\":" << count
        << ",\"message_type\":\"" << messageTypeName(type) << "\""
        << ",\"message_type_id\":" << static_cast<unsigned>(type)
        << ",\"sequence\":" << sequence
        << ",\"tls_active\":" << (tls_active ? "true" : "false")
        << "}\n";
}

const char* messageTypeName(protocol::MessageType type) {
    switch (type) {
        case protocol::MessageType::Query: return "Query";
        case protocol::MessageType::Sync: return "Sync";
        case protocol::MessageType::TxnCommit: return "TxnCommit";
        case protocol::MessageType::TxnRollback: return "TxnRollback";
        case protocol::MessageType::Ready: return "Ready";
        case protocol::MessageType::Error: return "Error";
        case protocol::MessageType::CommandComplete: return "CommandComplete";
        case protocol::MessageType::ParameterStatus: return "ParameterStatus";
        case protocol::MessageType::Notification: return "Notification";
        case protocol::MessageType::TxnStatus: return "TxnStatus";
        case protocol::MessageType::CopyInResponse: return "CopyInResponse";
        case protocol::MessageType::CopyOutResponse: return "CopyOutResponse";
        case protocol::MessageType::CopyBothResponse: return "CopyBothResponse";
        case protocol::MessageType::CopyData: return "CopyData";
        case protocol::MessageType::CopyDone: return "CopyDone";
        case protocol::MessageType::CopyFail: return "CopyFail";
        default: return "Other";
    }
}

bool isAsyncMessageType(protocol::MessageType type) {
    return type == protocol::MessageType::ParameterStatus ||
           type == protocol::MessageType::Notification ||
           type == protocol::MessageType::QueryPlan ||
           type == protocol::MessageType::SblrCompiled;
}

void traceWireEvent(const char* stage,
                    protocol::MessageType type,
                    uint32_t sequence,
                    bool in_transaction,
                    const char* detail = nullptr) {
    if (!wireDebugEnabled()) {
        return;
    }
    std::fprintf(stderr,
                 "[driver_wire] stage=%s type=%s(%u) seq=%u in_txn=%d detail=%s\n",
                 stage ? stage : "<null>",
                 messageTypeName(type),
                 static_cast<unsigned>(type),
                 sequence,
                 in_transaction ? 1 : 0,
                 detail ? detail : "<none>");
    std::fflush(stderr);
}

core::Status setError(core::ErrorContext* ctx, core::Status status, const std::string& message) {
    if (ctx) {
        ctx->set(status, message.c_str(), __FILE__, __LINE__, __func__);
    }
    return status;
}

core::Status setFeatureNotSupported(core::ErrorContext* ctx, const char* message) {
    if (ctx) {
        ctx->set(core::Status::NOT_IMPLEMENTED, message, __FILE__, __LINE__, __func__);
        ctx->setSQLState(core::SQLSTATE_FEATURE_NOT_SUPPORTED);
    }
    return core::Status::NOT_IMPLEMENTED;
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool isNativeProtocol(const std::string& value) {
    std::string lower = toLower(value);
    return lower.empty() || lower == "native";
}

bool isManagerProxyMode(const std::string& value) {
    std::string lower = toLower(value);
    return lower == "manager_proxy" || lower == "manager-proxy" || lower == "managed";
}

std::string firstSqlTokenUpper(std::string_view sql) {
    std::size_t i = 0;
    while (i < sql.size() && std::isspace(static_cast<unsigned char>(sql[i]))) {
        ++i;
    }
    std::string token;
    while (i < sql.size() &&
           (std::isalnum(static_cast<unsigned char>(sql[i])) || sql[i] == '_')) {
        token.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(sql[i]))));
        ++i;
    }
    return token;
}

bool canAdoptFreshNativeBoundary(const NetworkClient::TransactionOptions& options) {
    return options.flags == 0 &&
           options.conflict_action == 0 &&
           options.autocommit_mode == 0 &&
           options.isolation_level == 0 &&
           options.read_committed_mode == 0 &&
           options.access_mode == 0 &&
           options.deferrable == 0 &&
           options.wait_mode == 0 &&
           options.timeout_ms == 0;
}

bool querySupportsServerAutocommit(std::string_view sql) {
    const std::string token = firstSqlTokenUpper(sql);
    if (token == "BEGIN" ||
        token == "COMMIT" ||
        token == "COPY" ||
        token == "RELEASE" ||
        token == "ROLLBACK" ||
        token == "SAVEPOINT" ||
        token == "START") {
        return false;
    }
    return !token.empty();
}

std::vector<uint32_t> inferParameterTypesFromSql(std::string_view sql) {
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
        if (!in_single && !in_double && ch == '$' &&
            index + 1 < sql.size() &&
            std::isdigit(static_cast<unsigned char>(sql[index + 1]))) {
            std::size_t cursor = index + 1;
            std::size_t value = 0;
            while (cursor < sql.size() &&
                   std::isdigit(static_cast<unsigned char>(sql[cursor]))) {
                value = value * 10u + static_cast<std::size_t>(sql[cursor] - '0');
                ++cursor;
            }
            max_dollar_index = std::max(max_dollar_index, value);
            index = cursor;
            continue;
        }
        ++index;
    }
    const std::size_t count = std::max(question_count, max_dollar_index);
    return std::vector<uint32_t>(count, 0);
}

bool isManagedTransport(const std::string& value) {
    std::string lower = toLower(value);
    return lower == "managed" || lower == "manager" ||
           lower == "manager_proxy" || lower == "manager-proxy";
}

bool isInetTransport(const std::string& value) {
    std::string lower = toLower(value);
    return lower.empty() || lower == "inet_listener" || lower == "inet" ||
           lower == "listener" || lower == "tcp" || lower == "tcp_listener" ||
           lower == "network";
}

bool isLocalIpcTransport(const std::string& value) {
    std::string lower = toLower(value);
    return lower == "local_ipc" || lower == "local-ipc" || lower == "local" ||
           lower == "ipc" || lower == "unix" || lower == "unix_socket" ||
           lower == "unix-socket" || lower == "pipe";
}

bool isEmbeddedTransport(const std::string& value) {
    std::string lower = toLower(value);
    return lower == "embedded" || lower == "inproc" || lower == "in-process" ||
           lower == "in_process";
}

#if defined(SCRATCHBIRD_CLIENT_ENABLE_LOCAL_SBSQL_BRIDGE) && \
    defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
scratchbird::core::memory::AllocationPolicy embeddedClientMemoryPolicy() {
    scratchbird::core::memory::AllocationPolicy policy;
    policy.policy_name = "embedded_client_memory_policy";
    policy.hard_limit_bytes = 256ull * 1024ull * 1024ull;
    policy.soft_limit_bytes = 192ull * 1024ull * 1024ull;
    policy.per_context_limit_bytes = 96ull * 1024ull * 1024ull;
    policy.page_buffer_pool_limit_bytes = 64ull * 1024ull * 1024ull;
    policy.track_allocations = true;
    policy.zero_memory_on_release = true;
    return policy;
}

core::Status ensureEmbeddedClientMemoryManager(core::ErrorContext* ctx) {
    const auto state = scratchbird::core::memory::DefaultMemoryManagerState();
    if (state.initialized) {
        return core::Status::OK;
    }
    const auto installed = scratchbird::core::memory::ConfigureDefaultMemoryManager(
        embeddedClientMemoryPolicy(),
        "embedded_client_memory_policy");
    if (!installed.ok()) {
        const std::string code = installed.diagnostic.diagnostic_code.empty()
                                     ? "MEMORY.DEFAULT_MANAGER_CONFIGURATION_FAILED"
                                     : installed.diagnostic.diagnostic_code;
        return setError(ctx,
                        core::Status::CONNECTION_FAILURE,
                        "embedded client memory manager configuration failed: " + code);
    }
    return core::Status::OK;
}
#endif

std::string normalizeUnixEndpointPath(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
    if (value.rfind("unix:", 0) == 0) {
        return value.substr(5);
    }
    return value;
}

std::string sbpsEndpointFromIpcPath(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "unix:";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    std::string path = value.substr(first, last - first + 1);
    if (path.rfind("unix:", 0) == 0) {
        return path;
    }
    return "unix:" + path;
}

std::string joinMethodList(const std::vector<std::string>& methods) {
    std::ostringstream out;
    bool first = true;
    for (const auto& method : methods) {
        if (method.empty()) {
            continue;
        }
        if (!first) {
            out << ",";
        }
        out << method;
        first = false;
    }
    return out.str();
}

bool hasPinningOverlap(const std::vector<std::string>& required,
                       const std::vector<std::string>& forbidden,
                       std::string& overlap_out) {
    for (const auto& req : required) {
        const std::string req_norm = toLower(req);
        if (req_norm.empty()) {
            continue;
        }
        for (const auto& forbidden_method : forbidden) {
            if (req_norm == toLower(forbidden_method)) {
                overlap_out = req_norm;
                return true;
            }
        }
    }
    overlap_out.clear();
    return false;
}

core::Status resolveConnectionAddress(const NetworkClientConfig& config,
                                      network::NetworkAddress& address_out,
                                      core::ErrorContext* ctx) {
    std::string transport = toLower(config.transport_mode);
    if (transport.empty()) {
        transport = "inet_listener";
    }

    if (isManagedTransport(transport) || isInetTransport(transport)) {
        address_out.family = network::AddressFamily::IPV4;
        address_out.host = config.host.empty() ? "127.0.0.1" : config.host;
        address_out.port = config.port;
        return core::Status::OK;
    }

    return setError(ctx,
                    core::Status::INVALID_ARGUMENT,
                    "transport_mode must be embedded, inet_listener, local_ipc, or managed");
}

void appendU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void appendU32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void storeU32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void storeU64(uint8_t* out, uint64_t value) {
    for (size_t index = 0; index < 8; ++index) {
        out[index] = static_cast<uint8_t>((value >> (index * 8)) & 0xFF);
    }
}

uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readU32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

void appendLengthPrefixedString(std::vector<uint8_t>& out, const std::string& value) {
    appendU32(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

std::string base64Encode(const std::vector<uint8_t>& data) {
    if (data.empty()) {
        return "";
    }
    std::string out;
    out.resize(4 * ((data.size() + 2) / 3));
    int len = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]),
                              data.data(),
                              static_cast<int>(data.size()));
    if (len < 0) {
        return "";
    }
    out.resize(static_cast<size_t>(len));
    return out;
}

std::vector<uint8_t> base64Decode(const std::string& input) {
    if (input.empty()) {
        return {};
    }
    size_t out_len = (input.size() / 4) * 3;
    std::vector<uint8_t> out(out_len);
    int len = EVP_DecodeBlock(out.data(),
                              reinterpret_cast<const unsigned char*>(input.data()),
                              static_cast<int>(input.size()));
    if (len < 0) {
        return {};
    }
    size_t padding = 0;
    if (!input.empty() && input.back() == '=') {
        padding++;
        if (input.size() > 1 && input[input.size() - 2] == '=') {
            padding++;
        }
    }
    size_t final_len = static_cast<size_t>(len);
    if (final_len >= padding) {
        final_len -= padding;
    }
    out.resize(final_len);
    return out;
}

std::string trimWhitespace(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

struct BinaryCopyField {
    std::string name;
    bool is_null{false};
    std::string value;
};

using BinaryCopyRow = std::vector<BinaryCopyField>;

bool equalsIgnoreAsciiCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

bool parseCanonicalCopyLine(std::string_view line, BinaryCopyRow& row) {
    row.clear();
    const std::string trimmed = trimWhitespace(std::string(line));
    if (trimmed.empty()) {
        return true;
    }
    size_t start = 0;
    while (start <= trimmed.size()) {
        const size_t end = trimmed.find(';', start);
        const std::string_view field(trimmed.data() + start,
                                     (end == std::string::npos ? trimmed.size() : end) - start);
        if (!field.empty()) {
            const size_t eq = field.find('=');
            if (eq == std::string::npos || eq == 0) {
                return false;
            }
            BinaryCopyField parsed;
            parsed.name.assign(field.substr(0, eq));
            parsed.value.assign(field.substr(eq + 1));
            parsed.is_null = equalsIgnoreAsciiCase(parsed.value, "NULL");
            if (parsed.is_null) {
                parsed.value.clear();
            }
            row.push_back(std::move(parsed));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return !row.empty();
}

void appendLe32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
}

std::vector<uint8_t> buildBinaryCopyFrame(const std::vector<BinaryCopyRow>& rows) {
    std::vector<uint8_t> out;
    out.reserve(12 + rows.size() * 32);
    out.push_back('S');
    out.push_back('B');
    out.push_back('C');
    out.push_back('P');
    out.push_back(1);
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    appendLe32(out, static_cast<uint32_t>(rows.size()));
    for (const auto& row : rows) {
        appendLe32(out, static_cast<uint32_t>(row.size()));
        for (const auto& field : row) {
            appendLe32(out, static_cast<uint32_t>(field.name.size()));
            out.insert(out.end(), field.name.begin(), field.name.end());
            out.push_back(field.is_null ? 1 : 0);
            appendLe32(out, static_cast<uint32_t>(field.value.size()));
            out.insert(out.end(), field.value.begin(), field.value.end());
        }
    }
    return out;
}

std::string authMethodName(protocol::AuthMethod method) {
    switch (method) {
        case protocol::AuthMethod::Password:
            return "PASSWORD";
        case protocol::AuthMethod::Md5:
            return "MD5";
        case protocol::AuthMethod::ScramSha256:
            return "SCRAM_SHA_256";
        case protocol::AuthMethod::ScramSha512:
            return "SCRAM_SHA_512";
        case protocol::AuthMethod::Token:
            return "TOKEN";
        case protocol::AuthMethod::Peer:
            return "PEER";
        case protocol::AuthMethod::Reattach:
            return "REATTACH";
        default:
            return "";
    }
}

std::string defaultAuthPluginId(protocol::AuthMethod method) {
    switch (method) {
        case protocol::AuthMethod::Password:
            return "scratchbird.auth.password_compat";
        case protocol::AuthMethod::Md5:
            return "scratchbird.auth.md5_legacy";
        case protocol::AuthMethod::ScramSha256:
            return "scratchbird.auth.scram_sha_256";
        case protocol::AuthMethod::ScramSha512:
            return "scratchbird.auth.scram_sha_512";
        case protocol::AuthMethod::Token:
            return "scratchbird.auth.authkey_token";
        case protocol::AuthMethod::Peer:
            return "scratchbird.auth.peer_uid";
        case protocol::AuthMethod::Reattach:
            return "scratchbird.auth.reattach";
        default:
            return "";
    }
}

std::string authPluginIdForMethod(protocol::AuthMethod method,
                                  const std::string& configured_method_id) {
    const std::string trimmed = trimWhitespace(configured_method_id);
    if (!trimmed.empty()) {
        return trimmed;
    }
    return defaultAuthPluginId(method);
}

bool authMethodExecutableLocally(protocol::AuthMethod method) {
    switch (method) {
        case protocol::AuthMethod::Password:
        case protocol::AuthMethod::ScramSha256:
        case protocol::AuthMethod::ScramSha512:
        case protocol::AuthMethod::Token:
            return true;
        default:
            return false;
    }
}

bool authMethodBrokerRequired(protocol::AuthMethod method) {
    return method == protocol::AuthMethod::Peer;
}

bool describeAuthMethod(protocol::AuthMethod method,
                        const std::string& configured_method_id,
                        AuthMethodSurface& out) {
    const std::string wire_method = authMethodName(method);
    if (wire_method.empty()) {
        return false;
    }
    out.wire_method = wire_method;
    out.plugin_method_id = authPluginIdForMethod(method, configured_method_id);
    out.executable_locally = authMethodExecutableLocally(method);
    out.broker_required = authMethodBrokerRequired(method);
    return true;
}

core::Status resolveTokenAuthPayload(const NetworkClientConfig& config,
                                     std::vector<uint8_t>& payload_out,
                                     core::ErrorContext* ctx) {
    if (!trimWhitespace(config.auth_token).empty()) {
        payload_out.assign(config.auth_token.begin(), config.auth_token.end());
        return core::Status::OK;
    }
    if (!config.auth_method_payload.empty()) {
        payload_out.assign(config.auth_method_payload.begin(), config.auth_method_payload.end());
        return core::Status::OK;
    }
    if (!config.auth_payload_b64.empty()) {
        const std::string encoded = trimWhitespace(config.auth_payload_b64);
        if (!encoded.empty()) {
            auto decoded = base64Decode(encoded);
            if (!decoded.empty()) {
                payload_out = std::move(decoded);
                return core::Status::OK;
            }
        }
        return setError(ctx,
                        core::Status::INVALID_AUTHORIZATION,
                        "invalid auth_payload_b64 encoding");
    }
    if (!config.auth_payload_json.empty()) {
        payload_out.assign(config.auth_payload_json.begin(), config.auth_payload_json.end());
        return core::Status::OK;
    }
    if (!config.workload_identity_token.empty()) {
        payload_out.assign(config.workload_identity_token.begin(),
                           config.workload_identity_token.end());
        return core::Status::OK;
    }
    if (!config.proxy_principal_assertion.empty()) {
        payload_out.assign(config.proxy_principal_assertion.begin(),
                           config.proxy_principal_assertion.end());
        return core::Status::OK;
    }
    return setError(ctx,
                    core::Status::INVALID_AUTHORIZATION,
                    "TOKEN authentication requires auth_token, auth_method_payload, auth_payload_json, auth_payload_b64, workload_identity_token, or proxy_principal_assertion");
}

std::string escapeScram(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '=') {
            out += "=3D";
        } else if (c == ',') {
            out += "=2C";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string generateNonce() {
    std::vector<uint8_t> buf(18);
    if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1) {
        return "";
    }
    return base64Encode(buf);
}

const EVP_MD* scramDigest(protocol::AuthMethod method) {
    switch (method) {
        case protocol::AuthMethod::ScramSha512:
            return EVP_sha512();
        case protocol::AuthMethod::ScramSha256:
        default:
            return EVP_sha256();
    }
}

bool scramSaltedPassword(const std::string& password,
                         const std::vector<uint8_t>& salt,
                         uint32_t iterations,
                         protocol::AuthMethod method,
                         std::vector<uint8_t>& out) {
    const EVP_MD* md = scramDigest(method);
    int hash_len = EVP_MD_size(md);
    out.assign(static_cast<size_t>(hash_len), 0);
    if (PKCS5_PBKDF2_HMAC(password.c_str(),
                          static_cast<int>(password.size()),
                          salt.data(),
                          static_cast<int>(salt.size()),
                          static_cast<int>(iterations),
                          md,
                          hash_len,
                          out.data()) != 1) {
        return false;
    }
    return true;
}

bool scramHmac(const std::vector<uint8_t>& key,
               const std::string& message,
               protocol::AuthMethod method,
               std::vector<uint8_t>& out) {
    const EVP_MD* md = scramDigest(method);
    unsigned int out_len = static_cast<unsigned int>(EVP_MD_size(md));
    out.assign(out_len, 0);
    if (!HMAC(md,
              key.data(),
              static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char*>(message.data()),
              message.size(),
              out.data(),
              &out_len)) {
        return false;
    }
    out.resize(out_len);
    return true;
}

bool scramHash(const std::vector<uint8_t>& input,
               protocol::AuthMethod method,
               std::vector<uint8_t>& out) {
    const EVP_MD* md = scramDigest(method);
    unsigned int out_len = static_cast<unsigned int>(EVP_MD_size(md));
    out.assign(out_len, 0);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return false;
    }
    bool ok = EVP_DigestInit_ex(ctx, md, nullptr) == 1;
    ok = ok && (EVP_DigestUpdate(ctx, input.data(), input.size()) == 1);
    ok = ok && (EVP_DigestFinal_ex(ctx, out.data(), &out_len) == 1);
    EVP_MD_CTX_free(ctx);
    if (!ok) {
        return false;
    }
    out.resize(out_len);
    return true;
}

struct ScramClient {
    std::string client_nonce;
    std::string client_first_bare;
    std::vector<uint8_t> server_signature;
    protocol::AuthMethod method{protocol::AuthMethod::ScramSha256};

    std::string clientFirst(const std::string& username) {
        client_nonce = generateNonce();
        client_first_bare = "n=" + escapeScram(username) + ",r=" + client_nonce;
        return "n,," + client_first_bare;
    }

    bool handleServerFirst(const std::string& password,
                           const std::string& server_first,
                           std::string& client_final,
                           std::string& error) {
        size_t r_pos = server_first.find("r=");
        size_t s_pos = server_first.find(",s=");
        size_t i_pos = server_first.find(",i=");
        if (r_pos != 0 || s_pos == std::string::npos || i_pos == std::string::npos) {
            error = "Invalid SCRAM server-first";
            return false;
        }
        std::string nonce = server_first.substr(2, s_pos - 2);
        std::string salt_b64 = server_first.substr(s_pos + 3, i_pos - (s_pos + 3));
        std::string iter_str = server_first.substr(i_pos + 3);
        if (nonce.rfind(client_nonce, 0) != 0) {
            error = "SCRAM nonce mismatch";
            return false;
        }
        uint32_t iterations = 0;
        try {
            iterations = static_cast<uint32_t>(std::stoul(iter_str));
        } catch (...) {
            error = "SCRAM invalid iteration count";
            return false;
        }
        auto salt = base64Decode(salt_b64);
        if (salt.empty()) {
            error = "SCRAM invalid salt";
            return false;
        }
        std::vector<uint8_t> salted;
        if (!scramSaltedPassword(password, salt, iterations, method, salted)) {
            error = "SCRAM salted password failed";
            return false;
        }
        std::vector<uint8_t> client_key;
        if (!scramHmac(salted, "Client Key", method, client_key)) {
            error = "SCRAM client key failed";
            return false;
        }
        std::vector<uint8_t> stored_key;
        if (!scramHash(client_key, method, stored_key)) {
            error = "SCRAM stored key failed";
            return false;
        }
        std::string client_final_without_proof = "c=biws,r=" + nonce;
        std::string auth_message = client_first_bare + "," + server_first + "," + client_final_without_proof;
        std::vector<uint8_t> client_signature;
        if (!scramHmac(stored_key, auth_message, method, client_signature)) {
            error = "SCRAM client signature failed";
            return false;
        }
        std::vector<uint8_t> client_proof(client_key.size());
        for (size_t i = 0; i < client_key.size(); ++i) {
            client_proof[i] = client_key[i] ^ client_signature[i];
        }
        std::vector<uint8_t> server_key;
        if (!scramHmac(salted, "Server Key", method, server_key)) {
            error = "SCRAM server key failed";
            return false;
        }
        if (!scramHmac(server_key, auth_message, method, server_signature)) {
            error = "SCRAM server signature failed";
            return false;
        }
        client_final = client_final_without_proof + ",p=" + base64Encode(client_proof);
        return true;
    }

    bool verifyServerFinal(const std::string& server_final) const {
        if (server_final.rfind("v=", 0) != 0) {
            return false;
        }
        auto expected = base64Encode(server_signature);
        return server_final.substr(2) == expected;
    }
};

std::string buildSchemaStatement(const std::string& schema) {
    std::string trimmed = schema;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
    trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
    if (trimmed.empty()) {
        return "";
    }
    if (trimmed.find(',') == std::string::npos) {
        return "SET SCHEMA \"" + trimmed + "\"";
    }
    std::stringstream ss;
    ss << "SET SEARCH_PATH TO ";
    size_t start = 0;
    bool first = true;
    while (start < trimmed.size()) {
        size_t end = trimmed.find(',', start);
        if (end == std::string::npos) {
            end = trimmed.size();
        }
        std::string part = trimmed.substr(start, end - start);
        part.erase(0, part.find_first_not_of(" \t\n\r"));
        part.erase(part.find_last_not_of(" \t\n\r") + 1);
        if (!part.empty()) {
            if (!first) {
                ss << ", ";
            }
            ss << "\"" << part << "\"";
            first = false;
        }
        start = end + 1;
    }
    return ss.str();
}

core::Status mapProtocolError(const protocol::ProtocolMessage& msg,
                              core::ErrorContext* ctx) {
    std::string severity;
    std::string sqlstate;
    std::string message;
    std::string detail;
    std::string hint;
    auto status = protocol::parseErrorMessage(msg.body, severity, sqlstate, message, detail, hint, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    core::Status mapped = core::Status::INTERNAL_ERROR;
    if (message.find("AUTH_CLIENT_PINNING_VIOLATION") != std::string::npos ||
        message.find("AUTH_METHOD_NOT_ALLOWED") != std::string::npos ||
        message.find("AUTH_NO_LOGIN_DIRECT") != std::string::npos) {
        mapped = core::Status::INVALID_AUTHORIZATION;
    } else
    if (sqlstate.rfind("08", 0) == 0) {
        mapped = core::Status::CONNECTION_FAILURE;
    } else if (sqlstate == "28P01" || sqlstate == "28000") {
        mapped = core::Status::INVALID_PASSWORD;
    } else if (sqlstate == "25P01") {
        mapped = core::Status::NO_ACTIVE_TRANSACTION;
    } else if (sqlstate == "25P02") {
        mapped = core::Status::TRANSACTION_ABORTED;
    } else if (sqlstate == "25006") {
        mapped = core::Status::READ_ONLY_TRANSACTION;
    } else if (sqlstate.rfind("25", 0) == 0 || sqlstate == "2D000" || sqlstate == "0B000") {
        mapped = core::Status::INVALID_TRANSACTION_STATE;
    } else if (sqlstate.rfind("42", 0) == 0) {
        mapped = core::Status::SYNTAX_ERROR;
    } else if (sqlstate.rfind("23", 0) == 0) {
        mapped = core::Status::CONSTRAINT_VIOLATION;
    } else if (sqlstate == "40001") {
        mapped = core::Status::SERIALIZATION_FAILURE;
    } else if (sqlstate == "40P01") {
        mapped = core::Status::DEADLOCK;
    } else if (sqlstate == "57014") {
        mapped = core::Status::QUERY_CANCELED;
    } else if (sqlstate == "0A000") {
        mapped = core::Status::NOT_SUPPORTED;
    }
    if (!detail.empty()) {
        message += " (" + detail + ")";
    }
    if (ctx) {
        ctx->code = mapped;
        ctx->message = message;
        ctx->hint = hint;
        if (!sqlstate.empty()) {
            ctx->setSQLState(sqlstate.c_str());
        }
    }
    return mapped;
}

core::Status parseReadyAndTrackTransaction(const std::vector<uint8_t>& payload,
                                           bool& in_transaction,
                                           uint64_t& current_txn_id,
                                           core::ErrorContext* ctx) {
    uint8_t status_byte = 0;
    uint64_t txn_id = 0;
    uint64_t epoch = 0;
    auto status = protocol::parseReady(payload, status_byte, txn_id, epoch, ctx);
    if (status == core::Status::OK) {
        // READY status is authoritative for transaction activity. Attached
        // MGA sessions are always expected to expose an active boundary.
        if (status_byte == 0 || txn_id == 0) {
            in_transaction = false;
            current_txn_id = 0;
            return setError(ctx,
                            core::Status::PROTOCOL_VIOLATION,
                            "READY frame did not publish an active MGA transaction boundary");
        }
        in_transaction = true;
        current_txn_id = txn_id;
    }
    return status;
}

std::vector<uint8_t> parseUuidHex(const std::string& value) {
    std::string trimmed;
    trimmed.reserve(32);
    for (char c : value) {
        if (c == '-') {
            continue;
        }
        trimmed.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (trimmed.size() != 32) {
        return {};
    }
    std::vector<uint8_t> out(16);
    for (size_t i = 0; i < 16; ++i) {
        char hi = trimmed[i * 2];
        char lo = trimmed[i * 2 + 1];
        auto hexToNibble = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            return -1;
        };
        int high = hexToNibble(hi);
        int low = hexToNibble(lo);
        if (high < 0 || low < 0) {
            return {};
        }
        out[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return out;
}

} // namespace

NetworkPreparedStatement::NetworkPreparedStatement() = default;
NetworkPreparedStatement::~NetworkPreparedStatement() = default;

NetworkPreparedStatement::NetworkPreparedStatement(NetworkPreparedStatement&& other) noexcept = default;
NetworkPreparedStatement& NetworkPreparedStatement::operator=(NetworkPreparedStatement&& other) noexcept = default;

size_t NetworkPreparedStatement::getParameterCount() const {
    return param_count_;
}

bool NetworkPreparedStatement::isValid() const {
    return valid_;
}

void NetworkPreparedStatement::clearParameters() {
    for (auto& param : params_) {
        param.data.clear();
        param.is_null = true;
    }
}

void NetworkPreparedStatement::setNull(size_t index) {
    if (index == 0) {
        return;
    }
    if (params_.size() < index) {
        params_.resize(index);
        param_type_oids_.resize(index);
    }
    params_[index - 1].is_null = true;
}

void NetworkPreparedStatement::setNull(size_t index, uint32_t type_oid) {
    if (index == 0) {
        return;
    }
    if (params_.size() < index) {
        params_.resize(index);
        param_type_oids_.resize(index);
    }
    params_[index - 1].is_null = true;
    params_[index - 1].type_oid = type_oid;
    param_type_oids_[index - 1] = type_oid;
}

void NetworkPreparedStatement::setBool(size_t index, bool value) {
    if (index == 0) {
        return;
    }
    if (params_.size() < index) {
        params_.resize(index);
        param_type_oids_.resize(index);
    }
    params_[index - 1].is_null = false;
    params_[index - 1].format = protocol::kFormatBinary;
    params_[index - 1].type_oid = protocol::kOidBool;
    params_[index - 1].data = { static_cast<uint8_t>(value ? 1 : 0) };
    param_type_oids_[index - 1] = protocol::kOidBool;
}

void NetworkPreparedStatement::setInt16(size_t index, int16_t value) {
    if (index == 0) {
        return;
    }
    if (params_.size() < index) {
        params_.resize(index);
        param_type_oids_.resize(index);
    }
    params_[index - 1].is_null = false;
    params_[index - 1].format = protocol::kFormatBinary;
    params_[index - 1].type_oid = protocol::kOidInt2;
    params_[index - 1].data.resize(2);
    params_[index - 1].data[0] = static_cast<uint8_t>(value & 0xFF);
    params_[index - 1].data[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    param_type_oids_[index - 1] = protocol::kOidInt2;
}

void NetworkPreparedStatement::setInt32(size_t index, int32_t value) {
    if (index == 0) {
        return;
    }
    if (params_.size() < index) {
        params_.resize(index);
        param_type_oids_.resize(index);
    }
    params_[index - 1].is_null = false;
    params_[index - 1].format = protocol::kFormatBinary;
    params_[index - 1].type_oid = protocol::kOidInt4;
    params_[index - 1].data.resize(4);
    params_[index - 1].data[0] = static_cast<uint8_t>(value & 0xFF);
    params_[index - 1].data[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    params_[index - 1].data[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    params_[index - 1].data[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
    param_type_oids_[index - 1] = protocol::kOidInt4;
}

void NetworkPreparedStatement::setInt64(size_t index, int64_t value) {
    if (index == 0) {
        return;
    }
    if (params_.size() < index) {
        params_.resize(index);
        param_type_oids_.resize(index);
    }
    params_[index - 1].is_null = false;
    params_[index - 1].format = protocol::kFormatBinary;
    params_[index - 1].type_oid = protocol::kOidInt8;
    params_[index - 1].data.resize(8);
    for (size_t i = 0; i < 8; ++i) {
        params_[index - 1].data[i] = static_cast<uint8_t>((static_cast<uint64_t>(value) >> (8 * i)) & 0xFF);
    }
    param_type_oids_[index - 1] = protocol::kOidInt8;
}

void NetworkPreparedStatement::setFloat(size_t index, float value) {
    if (index == 0) {
        return;
    }
    if (params_.size() < index) {
        params_.resize(index);
        param_type_oids_.resize(index);
    }
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    params_[index - 1].is_null = false;
    params_[index - 1].format = protocol::kFormatBinary;
    params_[index - 1].type_oid = protocol::kOidFloat4;
    params_[index - 1].data.resize(4);
    params_[index - 1].data[0] = static_cast<uint8_t>(bits & 0xFF);
    params_[index - 1].data[1] = static_cast<uint8_t>((bits >> 8) & 0xFF);
    params_[index - 1].data[2] = static_cast<uint8_t>((bits >> 16) & 0xFF);
    params_[index - 1].data[3] = static_cast<uint8_t>((bits >> 24) & 0xFF);
    param_type_oids_[index - 1] = protocol::kOidFloat4;
}

void NetworkPreparedStatement::setDouble(size_t index, double value) {
    if (index == 0) {
        return;
    }
    if (params_.size() < index) {
        params_.resize(index);
        param_type_oids_.resize(index);
    }
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    params_[index - 1].is_null = false;
    params_[index - 1].format = protocol::kFormatBinary;
    params_[index - 1].type_oid = protocol::kOidFloat8;
    params_[index - 1].data.resize(8);
    for (size_t i = 0; i < 8; ++i) {
        params_[index - 1].data[i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xFF);
    }
    param_type_oids_[index - 1] = protocol::kOidFloat8;
}

void NetworkPreparedStatement::setString(size_t index, const std::string& value) {
    setString(index, value, protocol::kOidText);
}

void NetworkPreparedStatement::setString(size_t index, const std::string& value, uint32_t type_oid) {
    if (index == 0) {
        return;
    }
    if (params_.size() < index) {
        params_.resize(index);
        param_type_oids_.resize(index);
    }
    std::vector<uint8_t> data(value.begin(), value.end());
    params_[index - 1].is_null = false;
    params_[index - 1].format = protocol::kFormatBinary;
    params_[index - 1].type_oid = type_oid;
    params_[index - 1].data = std::move(data);
    param_type_oids_[index - 1] = type_oid;
}

void NetworkPreparedStatement::setBytes(size_t index, const std::vector<uint8_t>& value) {
    setBytes(index, value.data(), value.size());
}

void NetworkPreparedStatement::setBytes(size_t index, const uint8_t* data, size_t length) {
    if (index == 0) {
        return;
    }
    if (params_.size() < index) {
        params_.resize(index);
        param_type_oids_.resize(index);
    }
    std::vector<uint8_t> bytes;
    if (data && length > 0) {
        bytes.assign(data, data + length);
    }
    params_[index - 1].is_null = false;
    params_[index - 1].format = protocol::kFormatBinary;
    params_[index - 1].type_oid = protocol::kOidBytea;
    params_[index - 1].data = std::move(bytes);
    param_type_oids_[index - 1] = protocol::kOidBytea;
}

void NetworkPreparedStatement::setBinary(size_t index, const uint8_t* data, size_t length, uint32_t type_oid, bool length_prefixed) {
    (void)length_prefixed;
    if (index == 0) {
        return;
    }
    if (params_.size() < index) {
        params_.resize(index);
        param_type_oids_.resize(index);
    }
    std::vector<uint8_t> bytes;
    if (data && length > 0) {
        bytes.assign(data, data + length);
    }
    params_[index - 1].is_null = false;
    params_[index - 1].format = protocol::kFormatBinary;
    params_[index - 1].type_oid = type_oid;
    params_[index - 1].data = std::move(bytes);
    param_type_oids_[index - 1] = type_oid;
}

void NetworkPreparedStatement::setTimestamp(size_t index, int64_t microseconds) {
    if (index == 0) {
        return;
    }
    if (params_.size() < index) {
        params_.resize(index);
        param_type_oids_.resize(index);
    }
    int64_t micros_since_2000 = microseconds - (kDaysFrom1970To2000 * kMicrosPerDay);
    params_[index - 1].is_null = false;
    params_[index - 1].format = protocol::kFormatBinary;
    params_[index - 1].type_oid = protocol::kOidTimestamptz;
    params_[index - 1].data.resize(8);
    for (size_t i = 0; i < 8; ++i) {
        params_[index - 1].data[i] = static_cast<uint8_t>((static_cast<uint64_t>(micros_since_2000) >> (8 * i)) & 0xFF);
    }
    param_type_oids_[index - 1] = protocol::kOidTimestamptz;
}

void NetworkPreparedStatement::setDate(size_t index, int32_t days) {
    if (index == 0) {
        return;
    }
    if (params_.size() < index) {
        params_.resize(index);
        param_type_oids_.resize(index);
    }
    params_[index - 1].is_null = false;
    params_[index - 1].format = protocol::kFormatBinary;
    params_[index - 1].type_oid = protocol::kOidDate;
    params_[index - 1].data.resize(4);
    params_[index - 1].data[0] = static_cast<uint8_t>(days & 0xFF);
    params_[index - 1].data[1] = static_cast<uint8_t>((days >> 8) & 0xFF);
    params_[index - 1].data[2] = static_cast<uint8_t>((days >> 16) & 0xFF);
    params_[index - 1].data[3] = static_cast<uint8_t>((days >> 24) & 0xFF);
    param_type_oids_[index - 1] = protocol::kOidDate;
}

void NetworkPreparedStatement::setTime(size_t index, int64_t microseconds) {
    if (index == 0) {
        return;
    }
    if (params_.size() < index) {
        params_.resize(index);
        param_type_oids_.resize(index);
    }
    params_[index - 1].is_null = false;
    params_[index - 1].format = protocol::kFormatBinary;
    params_[index - 1].type_oid = protocol::kOidTime;
    params_[index - 1].data.resize(8);
    for (size_t i = 0; i < 8; ++i) {
        params_[index - 1].data[i] = static_cast<uint8_t>((static_cast<uint64_t>(microseconds) >> (8 * i)) & 0xFF);
    }
    param_type_oids_[index - 1] = protocol::kOidTime;
}

void NetworkPreparedStatement::setUUID(size_t index, const std::vector<uint8_t>& value) {
    if (index == 0) {
        return;
    }
    if (params_.size() < index) {
        params_.resize(index);
        param_type_oids_.resize(index);
    }
    params_[index - 1].is_null = false;
    params_[index - 1].format = protocol::kFormatBinary;
    params_[index - 1].type_oid = protocol::kOidUuid;
    params_[index - 1].data = value;
    param_type_oids_[index - 1] = protocol::kOidUuid;
}

void NetworkPreparedStatement::setUUID(size_t index, const std::string& value) {
    auto bytes = parseUuidHex(value);
    if (bytes.empty()) {
        setNull(index, protocol::kOidUuid);
        return;
    }
    setUUID(index, bytes);
}

NetworkClient::NetworkClient() = default;
NetworkClient::~NetworkClient() {
    disconnect();
}

void NetworkClient::resetResolvedAuthContext() {
    resolved_auth_context_ = ResolvedAuthContext{};
    if (!config_.front_door_mode.empty()) {
        resolved_auth_context_.ingress_mode = isManagerProxyMode(config_.front_door_mode)
            ? "manager_proxy"
            : "direct";
    }
}

core::Status NetworkClient::openSocket(bool require_identity,
                                       bool require_manager_token,
                                       core::ErrorContext* ctx) {
    if (!isNativeProtocol(config_.protocol)) {
        return setError(ctx,
                        core::Status::INVALID_ARGUMENT,
                        "Only protocol=native is supported; configure the native parser listener.");
    }
    if (!config_.front_door_mode.empty() &&
        !isManagerProxyMode(config_.front_door_mode) &&
        toLower(config_.front_door_mode) != "direct") {
        return setError(ctx,
                        core::Status::INVALID_ARGUMENT,
                        "front_door_mode must be direct or manager_proxy");
    }
    if (isManagedTransport(config_.transport_mode) && !isManagerProxyMode(config_.front_door_mode)) {
        config_.front_door_mode = "manager_proxy";
    }
    if (isManagerProxyMode(config_.front_door_mode) && !isManagedTransport(config_.transport_mode)) {
        config_.transport_mode = "managed";
    }
    if (require_manager_token &&
        isManagerProxyMode(config_.front_door_mode) &&
        config_.manager_auth_token.empty()) {
        return setError(ctx,
                        core::Status::INVALID_ARGUMENT,
                        "manager_proxy mode requires manager_auth_token");
    }
    if (!config_.auth_method_id.empty() &&
        !protocol::isValidAuthMethodId(config_.auth_method_id)) {
        return setError(ctx, core::Status::INVALID_ARGUMENT,
                        "auth_method_id must start with scratchbird.auth.");
    }
    if (require_identity && config_.database.empty()) {
        return setError(ctx, core::Status::INVALID_ARGUMENT, "database is required");
    }
    if (isLocalIpcTransport(config_.transport_mode)) {
        return openLocalIpcBridge(ctx);
    }
    if (isEmbeddedTransport(config_.transport_mode)) {
        return openEmbeddedBridge(ctx);
    }

    network_guard_ = std::make_unique<network::NetworkInitGuard>();
    if (!network_guard_->isInitialized()) {
        network_guard_.reset();
        return setError(ctx, core::Status::CONNECTION_FAILURE, "Network init failed");
    }

    network::NetworkAddress address;
    core::Status status = resolveConnectionAddress(config_, address, ctx);
    if (status != core::Status::OK) {
        network_guard_.reset();
        return status;
    }

    network::SocketOptions options;
    options.connect_timeout_ms = config_.connect_timeout_ms;
    options.read_timeout_ms = config_.read_timeout_ms;
    options.write_timeout_ms = config_.write_timeout_ms;

    socket_ = network::Socket::connect(address, options, ctx);
    if (!socket_) {
        network_guard_.reset();
        return setError(ctx, core::Status::CONNECTION_FAILURE, "Connection failed");
    }

    if (config_.ssl_mode != network::SSLMode::DISABLED) {
        security::TLSClientConfig tls_config;
        tls_config.expected_hostname = config_.host;
        tls_config.sni_hostname = config_.host;
        tls_config.cert_file = config_.ssl_cert;
        tls_config.key_file = config_.ssl_key;
        tls_config.ca_file = config_.ssl_root_cert;
        tls_config.verify_server = (config_.ssl_mode == network::SSLMode::VERIFY_CA ||
                                    config_.ssl_mode == network::SSLMode::VERIFY_FULL);

        tls_ctx_ = security::TLSContext::createClient(tls_config, ctx);
        if (!tls_ctx_ || !tls_ctx_->isValid()) {
            disconnectSocketForReconnect();
            return setError(ctx, core::Status::CONNECTION_FAILURE, "TLS context init failed");
        }
        tls_conn_ = std::make_unique<security::TLSConnection>(*tls_ctx_);
        if (tls_conn_->setFd(socket_->getFd()) != core::Status::OK) {
            disconnectSocketForReconnect();
            return setError(ctx, core::Status::CONNECTION_FAILURE, "TLS socket attach failed");
        }
        if (tls_config.sni_hostname.size() > 0) {
            tls_conn_->setSNIHostname(tls_config.sni_hostname);
        }
        if (tls_conn_->connect() != core::Status::OK) {
            disconnectSocketForReconnect();
            return setError(ctx, core::Status::CONNECTION_FAILURE, "TLS handshake failed");
        }
        tls_active_ = true;
    } else {
        // Explicit sslmode=disable allows plain inet transport for local/test deployments.
        tls_active_ = false;
    }
    resetResolvedAuthContext();
    return core::Status::OK;
}

core::Status NetworkClient::openLocalIpcBridge(core::ErrorContext* ctx) {
    const std::string method = toLower(config_.ipc_method.empty() ? "auto" : config_.ipc_method);
    if (method != "auto" && method != "unix") {
        return setError(ctx,
                        core::Status::INVALID_ARGUMENT,
                        "local_ipc currently supports ipc_method=auto or ipc_method=unix; use inet_listener for tcp routes");
    }
    const std::string endpoint_path = normalizeUnixEndpointPath(config_.ipc_path);
    if (endpoint_path.empty()) {
        return setError(ctx,
                        core::Status::INVALID_ARGUMENT,
                        "ipc_path is required for local_ipc and must point to the running server SBPS endpoint");
    }

#if defined(SCRATCHBIRD_CLIENT_ENABLE_LOCAL_SBSQL_BRIDGE) && !defined(_WIN32)
    int fds[2]{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return setError(ctx,
                        core::Status::CONNECTION_FAILURE,
                        "socketpair() failed while opening local_ipc parser bridge");
    }

    auto client_socket = network::Socket::fromFd(fds[0],
                                                 network::AddressFamily::UNIX,
                                                 network::SocketType::STREAM);
    if (!client_socket) {
        ::close(fds[0]);
        ::close(fds[1]);
        return setError(ctx,
                        core::Status::CONNECTION_FAILURE,
                        "local_ipc parser bridge socket creation failed");
    }

    const int parser_fd = fds[1];
    const std::string server_endpoint = sbpsEndpointFromIpcPath(config_.ipc_path);
    try {
        local_ipc_bridge_thread_ = std::thread([server_endpoint, parser_fd]() {
            try {
                scratchbird::parser::sbsql::ParserMetrics metrics;
                scratchbird::parser::sbsql::SblrTemplateCache cache(10000);
                scratchbird::parser::sbsql::ParserConfig parser_config;
                parser_config.listener_worker = false;
                parser_config.probe_mode = false;
                parser_config.allow_probe_auth = false;
                parser_config.server_endpoint = server_endpoint;
                parser_config.dialect = "sbsql";
                parser_config.profile_id = "local_ipc";
                parser_config.bundle_contract_id = "sbp_sbsql_local_ipc@1";
                parser_config.parser_uuid = "local_ipc_client_bridge";
                parser_config.tls_required = false;
                scratchbird::parser::sbsql::SbsqlTestWireSession session(
                    std::move(parser_config),
                    &metrics,
                    &cache);
                (void)session.ServeFd(parser_fd);
            } catch (...) {
            }
            ::close(parser_fd);
        });
    } catch (const std::exception& exc) {
        client_socket->close();
        ::close(parser_fd);
        return setError(ctx,
                        core::Status::CONNECTION_FAILURE,
                        std::string("local_ipc parser bridge thread start failed: ") + exc.what());
    } catch (...) {
        client_socket->close();
        ::close(parser_fd);
        return setError(ctx,
                        core::Status::CONNECTION_FAILURE,
                        "local_ipc parser bridge thread start failed");
    }

    socket_ = std::move(client_socket);
    tls_active_ = false;
    tls_conn_.reset();
    tls_ctx_.reset();
    resetResolvedAuthContext();
    return core::Status::OK;
#else
    return setFeatureNotSupported(ctx,
                                  "local_ipc SBsql parser bridge support is not linked into this client build");
#endif
}

core::Status NetworkClient::openEmbeddedBridge(core::ErrorContext* ctx) {
#if defined(SCRATCHBIRD_CLIENT_ENABLE_LOCAL_SBSQL_BRIDGE) && \
    defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT) && !defined(_WIN32)
    if (config_.database.empty()) {
        return setError(ctx, core::Status::INVALID_ARGUMENT, "database is required for embedded mode");
    }
    if (const auto memory_status = ensureEmbeddedClientMemoryManager(ctx);
        memory_status != core::Status::OK) {
        return memory_status;
    }

    scratchbird::server::DatabaseOwnershipRequest ownership_request;
    ownership_request.database_path = config_.database;
    ownership_request.owner_kind = "embedded";
    auto ownership = scratchbird::server::AcquireDatabaseOwnership(ownership_request);
    if (!ownership.acquired) {
        if (!ownership.incumbent.sbps_endpoint.empty()) {
            config_.transport_mode = "local_ipc";
            config_.front_door_mode = "direct";
            config_.ipc_method = "unix";
            config_.ipc_path = ownership.incumbent.sbps_endpoint;
            return openLocalIpcBridge(ctx);
        }
        return setError(ctx,
                        core::Status::CONNECTION_FAILURE,
                        "embedded database is already owned and no controlling SBPS endpoint was published");
    }

    int fds[2]{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return setError(ctx,
                        core::Status::CONNECTION_FAILURE,
                        "socketpair() failed while opening embedded parser bridge");
    }

    auto client_socket = network::Socket::fromFd(fds[0],
                                                 network::AddressFamily::UNIX,
                                                 network::SocketType::STREAM);
    if (!client_socket) {
        ::close(fds[0]);
        ::close(fds[1]);
        return setError(ctx,
                        core::Status::CONNECTION_FAILURE,
                        "embedded parser bridge socket creation failed");
    }

    const int parser_fd = fds[1];
    const std::string database_path = config_.database;
    auto ownership_lock = std::move(ownership.lock);
    try {
        local_ipc_bridge_thread_ =
            std::thread([database_path, ownership_lock = std::move(ownership_lock), parser_fd]() {
                try {
                    (void)ownership_lock;
                    scratchbird::parser::sbsql::ParserMetrics metrics;
                    scratchbird::parser::sbsql::SblrTemplateCache cache(10000);
                    scratchbird::parser::sbsql::ParserConfig parser_config;
                    parser_config.listener_worker = false;
                    parser_config.probe_mode = false;
                    parser_config.allow_probe_auth = false;
                    parser_config.server_endpoint.clear();
                    parser_config.database_token = database_path;
                    parser_config.embedded_engine_direct = true;
                    parser_config.embedded_auth_bypass_sysarch = true;
                    parser_config.embedded_database_ownership_prelocked = true;
                    parser_config.embedded_database_path = database_path;
                    parser_config.dialect = "sbsql";
                    parser_config.profile_id = "embedded";
                    parser_config.bundle_contract_id = "sbp_sbsql_embedded@1";
                    parser_config.parser_uuid = "embedded_client_bridge";
                    parser_config.tls_required = false;
                    scratchbird::parser::sbsql::SbsqlTestWireSession session(
                        std::move(parser_config),
                        &metrics,
                        &cache);
                    (void)session.ServeFd(parser_fd);
                } catch (...) {
                }
                ::close(parser_fd);
            });
    } catch (const std::exception& exc) {
        client_socket->close();
        ::close(parser_fd);
        return setError(ctx,
                        core::Status::CONNECTION_FAILURE,
                        std::string("embedded parser bridge thread start failed: ") + exc.what());
    } catch (...) {
        client_socket->close();
        ::close(parser_fd);
        return setError(ctx,
                        core::Status::CONNECTION_FAILURE,
                        "embedded parser bridge thread start failed");
    }

    socket_ = std::move(client_socket);
    tls_active_ = false;
    tls_conn_.reset();
    tls_ctx_.reset();
    resetResolvedAuthContext();
    return core::Status::OK;
#else
    return setFeatureNotSupported(ctx,
                                  "embedded SBsql parser/engine support is not linked into this client build");
#endif
}

void NetworkClient::joinLocalIpcBridge() {
    if (local_ipc_bridge_thread_.joinable()) {
        local_ipc_bridge_thread_.join();
    }
}

void NetworkClient::disconnectSocketForReconnect() {
    if (tls_conn_) {
        tls_conn_->shutdown();
        tls_conn_.reset();
    }
    if (socket_) {
        socket_->close();
        socket_.reset();
    }
    joinLocalIpcBridge();
    tls_ctx_.reset();
    session_id_.fill(0);
    parameter_status_.clear();
    prepared_statements_.clear();
    notifications_.clear();
    last_plan_.reset();
    last_sblr_.reset();
    resetQueryProgress();
    next_sequence_ = 0;
    last_query_sequence_ = 0;
    in_transaction_ = false;
    explicit_transaction_active_ = false;
    server_autocommit_requested_ = false;
    current_txn_id_ = 0;
    resolved_auth_context_.attached = false;
    network_guard_.reset();
}

core::Status NetworkClient::probeAuthSurface(const NetworkClientConfig& config,
                                             AuthProbeResult& result,
                                             core::ErrorContext* ctx) {
    if (connected_) {
        return setError(ctx, core::Status::INVALID_ARGUMENT, "Already connected");
    }
    config_ = config;
    result = AuthProbeResult{};
    auto status = openSocket(false, false, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    const bool local_ipc = isLocalIpcTransport(config_.transport_mode);
    const std::string resolved_host = local_ipc
        ? normalizeUnixEndpointPath(config_.ipc_path)
        : (config_.host.empty() ? "127.0.0.1" : config_.host);
    const uint16_t resolved_port = local_ipc ? 0 : config_.port;
    result.reachable = true;
    result.ingress_mode = isManagerProxyMode(config_.front_door_mode) ? "manager_proxy" : "direct";
    result.resolved_host = resolved_host;
    result.resolved_port = resolved_port;
    if (isManagerProxyMode(config_.front_door_mode)) {
        status = probeManagerAuthSurface(result, ctx);
    } else {
        status = probeDirectAuthSurface(result, ctx);
    }
    disconnectSocketForReconnect();
    return status;
}

core::Status NetworkClient::connect(const NetworkClientConfig& config,
                                    core::ErrorContext* ctx) {
    if (connected_) {
        return setError(ctx, core::Status::INVALID_ARGUMENT, "Already connected");
    }
    config_ = config;
    core::Status status = openSocket(true, true, ctx);
    if (status != core::Status::OK) {
        return status;
    }

    if (isManagerProxyMode(config_.front_door_mode)) {
        status = performManagerConnect(ctx);
        if (status != core::Status::OK) {
            disconnectSocketForReconnect();
            return status;
        }
    }

    status = handshake(ctx);
    if (status != core::Status::OK) {
        disconnectSocketForReconnect();
        return status;
    }

    if (!config_.schema.empty() && config_.schema != "users.public") {
        std::string schema_stmt = buildSchemaStatement(config_.schema);
        if (!schema_stmt.empty()) {
            NetworkResultSet ignore;
            status = executeQuery(schema_stmt, ignore, ctx);
            if (status != core::Status::OK) {
                disconnectSocketForReconnect();
                return status;
            }
        }
    }

    connected_ = true;
    resolved_auth_context_.attached = true;
    return core::Status::OK;
}

void NetworkClient::disconnect() {
    disconnectSocketForReconnect();
    connected_ = false;
    in_transaction_ = false;
    explicit_transaction_active_ = false;
    server_autocommit_requested_ = false;
    current_txn_id_ = 0;
    last_commit_finality_ = NetworkTransactionFinality{};
    last_commit_finality_present_ = false;
    last_error_.clear();
}

core::Status NetworkClient::buildStartupParams(uint64_t& features_out,
                                               std::map<std::string, std::string>& params_out,
                                               core::ErrorContext* ctx) {
    std::string overlap_method;
    if (hasPinningOverlap(config_.auth_required_methods,
                          config_.auth_forbidden_methods,
                          overlap_method)) {
        return setError(ctx,
                        core::Status::INVALID_ARGUMENT,
                        "Invalid auth pinning profile: method appears in both required and forbidden sets (" +
                            overlap_method + ")");
    }

    features_out = protocol::kFeatureSblr |
                   protocol::kFeatureNotifications |
                   protocol::kFeatureSavepoints |
                   protocol::kFeatureQueryPlan;
    if (config_.enable_copy_streaming) {
        features_out |= protocol::kFeatureStreaming |
                        protocol::kFeatureBinaryCopy |
                        protocol::kFeatureBulkRejects;
    }
    if (config_.enable_compression) {
        features_out |= protocol::kFeatureCompression;
    }

    params_out.clear();
    params_out["database"] = config_.database;
    params_out["user"] = config_.username;
    if (!config_.role.empty()) {
        params_out["role"] = config_.role;
    }
    if (!config_.application_name.empty()) {
        params_out["application_name"] = config_.application_name;
    }
    params_out["client_flags"] = std::to_string(config_.connect_client_flags);
    if (!config_.auth_method_id.empty()) {
        params_out["auth_method_id"] = config_.auth_method_id;
    }
    if (!config_.auth_method_payload.empty()) {
        params_out["auth_method_payload"] = config_.auth_method_payload;
    }
    if (!config_.auth_payload_json.empty()) {
        params_out["auth_payload_json"] = config_.auth_payload_json;
    }
    if (!config_.auth_payload_b64.empty()) {
        params_out["auth_payload_b64"] = config_.auth_payload_b64;
    }
    if (!config_.auth_provider_profile.empty()) {
        params_out["auth_provider_profile"] = config_.auth_provider_profile;
    }
    const std::string required_methods = joinMethodList(config_.auth_required_methods);
    if (!required_methods.empty()) {
        params_out["auth_required_methods"] = required_methods;
    }
    const std::string forbidden_methods = joinMethodList(config_.auth_forbidden_methods);
    if (!forbidden_methods.empty()) {
        params_out["auth_forbidden_methods"] = forbidden_methods;
    }
    if (config_.auth_require_channel_binding) {
        params_out["auth_require_channel_binding"] = "1";
    }
    if (!config_.workload_identity_token.empty()) {
        params_out["workload_identity_token"] = config_.workload_identity_token;
    }
    if (!config_.proxy_principal_assertion.empty()) {
        params_out["proxy_principal_assertion"] = config_.proxy_principal_assertion;
    }
    return core::Status::OK;
}

core::Status NetworkClient::probeDirectAuthSurface(AuthProbeResult& result,
                                                   core::ErrorContext* ctx) {
    uint64_t features = 0;
    std::map<std::string, std::string> params;
    auto status = buildStartupParams(features, params, ctx);
    if (status != core::Status::OK) {
        return status;
    }

    const uint64_t required_features =
        config_.enable_copy_streaming
            ? (protocol::kFeatureStreaming | protocol::kFeatureBinaryCopy)
            : 0;
    auto payload = protocol::buildP1StartupPayload(features, required_features, params);
    status = sendMessage(protocol::MessageType::Startup, payload, 0, true, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }

    while (true) {
        protocol::ProtocolMessage msg;
        status = receiveMessage(msg, ctx);
        if (status != core::Status::OK) {
            return status;
        }
        if (handleAsyncMessage(msg, ctx) == core::Status::OK &&
            isAsyncMessageType(msg.header.type)) {
            continue;
        }
        switch (msg.header.type) {
            case protocol::MessageType::NegotiateVersion:
                continue;
            case protocol::MessageType::AuthRequest: {
                protocol::AuthMethod method{protocol::AuthMethod::Ok};
                std::vector<uint8_t> data;
                status = protocol::parseAuthRequest(msg.body, method, data, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                AuthMethodSurface surface;
                if (describeAuthMethod(method, config_.auth_method_id, surface)) {
                    result.admitted_methods = {surface};
                    result.required_method = surface.wire_method;
                    result.required_plugin_method_id = surface.plugin_method_id;
                }
                result.additional_continuation_possible =
                    method == protocol::AuthMethod::ScramSha256 ||
                    method == protocol::AuthMethod::ScramSha512 ||
                    method == protocol::AuthMethod::Token ||
                    method == protocol::AuthMethod::Peer;
                return core::Status::OK;
            }
            case protocol::MessageType::AuthOk:
            case protocol::MessageType::Ready:
                return core::Status::OK;
            case protocol::MessageType::Error:
                return mapProtocolError(msg, ctx);
            default:
                break;
        }
    }
}

core::Status NetworkClient::probeManagerAuthSurface(AuthProbeResult& result,
                                                    core::ErrorContext* ctx) {
    const std::string manager_user =
        config_.manager_username.empty() ? (config_.username.empty() ? "admin" : config_.username)
                                         : config_.manager_username;

    std::vector<uint8_t> hello_payload;
    hello_payload.reserve(4);
    appendU16(hello_payload, kMcpProtocolVersion);
    appendU16(hello_payload, config_.manager_client_flags);
    auto status = sendManagerFrame(kMsgMcpHello, hello_payload, ctx);
    if (status != core::Status::OK) {
        return status;
    }

    uint8_t msg_type = 0;
    std::vector<uint8_t> msg_payload;
    status = receiveManagerFrame(msg_type, msg_payload, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    if (msg_type != kMsgStatusResponse) {
        return setError(ctx, core::Status::PROTOCOL_VIOLATION,
                        "Expected MCP hello status response");
    }

    std::vector<uint8_t> auth_start;
    appendLengthPrefixedString(auth_start, manager_user);
    auth_start.push_back(kMcpAuthMethodToken);
    appendU32(auth_start, 0);
    status = sendManagerFrame(kMsgMcpAuthStart, auth_start, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    status = receiveManagerFrame(msg_type, msg_payload, ctx);
    if (status != core::Status::OK) {
        return status;
    }

    AuthMethodSurface surface;
    if (describeAuthMethod(protocol::AuthMethod::Token, "", surface)) {
        result.admitted_methods = {surface};
        result.required_method = surface.wire_method;
        result.required_plugin_method_id = surface.plugin_method_id;
    }
    result.additional_continuation_possible = (msg_type == kMsgAuthChallenge);
    return core::Status::OK;
}

bool NetworkClient::isConnected() const {
    return connected_;
}

core::Status NetworkClient::executeQuery(const std::string& sql,
                                         NetworkResultSet& results,
                                         core::ErrorContext* ctx) {
    results = NetworkResultSet{};
    const bool phase_trace = driverPhaseTraceEnabled();
    const int64_t execute_started = phase_trace ? driverPhaseNowNs() : 0;
    uint32_t seq = 0;
    if (wireDebugEnabled()) {
        std::fprintf(stderr,
                     "[driver_wire] stage=execute_query_send sql=%s in_txn=%d\n",
                     sql.c_str(),
                     in_transaction_ ? 1 : 0);
        std::fflush(stderr);
    }
    const bool copy_statement = firstSqlTokenUpper(sql) == "COPY";
    struct CopyHintLocalGuard {
        NetworkClient* client{nullptr};
        bool active{false};
        ~CopyHintLocalGuard() {
            if (active && client != nullptr) {
                client->setCopyInputSizeHintBytes(0);
                client->setCopyPreallocationFactorPercent(82);
            }
        }
    } copy_hint_local_guard{this, copy_statement};
    bool copy_hint_sent = false;
    auto consume_copy_hint = [&]() {
        if (copy_statement) {
            copy_input_size_hint_bytes_ = 0;
            copy_preallocation_factor_percent_ = 82;
        }
    };
    auto clear_server_copy_hint = [&]() -> core::Status {
        consume_copy_hint();
        if (!copy_hint_sent) {
            return core::Status::OK;
        }
        core::ErrorContext clear_ctx;
        auto clear_status = setOption("copy.source_size_bytes", "", &clear_ctx);
        if (clear_status != core::Status::OK) {
            return clear_status;
        }
        return setOption("copy.preallocation_factor_percent", "", &clear_ctx);
    };
    if (copy_statement && copy_input_size_hint_bytes_ != 0) {
        auto hint_status =
            setOption("copy.source_size_bytes",
                      std::to_string(copy_input_size_hint_bytes_),
                      ctx);
        if (hint_status != core::Status::OK) {
            consume_copy_hint();
            return hint_status;
        }
        copy_hint_sent = true;
        if (copy_preallocation_factor_percent_ != 0) {
            hint_status =
                setOption("copy.preallocation_factor_percent",
                          std::to_string(copy_preallocation_factor_percent_),
                          ctx);
            if (hint_status != core::Status::OK) {
                consume_copy_hint();
                return hint_status;
            }
        }
    }
    uint32_t query_flags = protocol::kQueryFlagBinaryResult;
    if (config_.autocommit && !explicit_transaction_active_ &&
        querySupportsServerAutocommit(sql)) {
        query_flags |= protocol::kQueryFlagAutocommit;
    }
    const int64_t build_started = phase_trace ? driverPhaseNowNs() : 0;
    auto payload = protocol::buildQueryPayload(sql, query_flags, 0, 0);
    if (phase_trace) {
        writeDriverPhaseTrace("execute_query",
                              "build_query_payload",
                              driverPhaseNowNs() - build_started,
                              payload.size(),
                              sql.size(),
                              protocol::MessageType::Query,
                              0,
                              tls_active_);
    }
    const int64_t send_started = phase_trace ? driverPhaseNowNs() : 0;
    auto status = sendMessage(protocol::MessageType::Query, payload, 0, false, &seq, ctx);
    if (phase_trace) {
        writeDriverPhaseTrace("execute_query",
                              "send_query",
                              driverPhaseNowNs() - send_started,
                              payload.size(),
                              1,
                              protocol::MessageType::Query,
                              seq,
                              tls_active_);
    }
    if (status != core::Status::OK) {
        server_autocommit_requested_ = false;
        return status;
    }
    last_query_sequence_ = seq;
    server_autocommit_requested_ = (query_flags & protocol::kQueryFlagAutocommit) != 0;

    std::vector<protocol::ColumnInfo> cols;
    while (true) {
        protocol::ProtocolMessage msg;
        const int64_t receive_started = phase_trace ? driverPhaseNowNs() : 0;
        status = receiveMessage(msg, ctx);
        if (status != core::Status::OK) {
            return status;
        }
        if (phase_trace) {
            writeDriverPhaseTrace("execute_query",
                                  "receive_message",
                                  driverPhaseNowNs() - receive_started,
                                  msg.body.size(),
                                  1,
                                  msg.header.type,
                                  msg.header.sequence,
                                  tls_active_);
        }
        traceWireEvent("execute_query_recv",
                       msg.header.type,
                       msg.header.sequence,
                       in_transaction_);
        if (handleAsyncMessage(msg, ctx) == core::Status::OK &&
            isAsyncMessageType(msg.header.type)) {
            continue;
        }
        switch (msg.header.type) {
            case protocol::MessageType::RowDescription: {
                cols.clear();
                status = protocol::parseRowDescription(msg.body, cols, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                results.columns.clear();
                results.columns.reserve(cols.size());
                for (const auto& col : cols) {
                    NetworkColumn out;
                    out.name = col.name;
                    out.type_oid = col.type_oid;
                    out.type_modifier = col.type_modifier;
                    out.format = col.format;
                    out.nullable = col.nullable;
                    results.columns.push_back(std::move(out));
                }
                break;
            }
            case protocol::MessageType::DataRow: {
                std::vector<protocol::ColumnValue> values;
                status = protocol::parseDataRow(msg.body, cols.size(), values, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                results.rows.push_back(std::move(values));
                break;
            }
            case protocol::MessageType::CopyInResponse: {
                protocol::CopyInResponse response;
                const int64_t parse_started = phase_trace ? driverPhaseNowNs() : 0;
                status = protocol::parseCopyInResponse(msg.body, response, ctx);
                if (phase_trace) {
                    writeDriverPhaseTrace("execute_query",
                                          "parse_copy_in_response",
                                          driverPhaseNowNs() - parse_started,
                                          msg.body.size(),
                                          0,
                                          msg.header.type,
                                          msg.header.sequence,
                                          tls_active_);
                }
                if (status != core::Status::OK) {
                    return status;
                }
                const int64_t copy_started = phase_trace ? driverPhaseNowNs() : 0;
                status = sendCopyInputStream(response, ctx);
                if (phase_trace) {
                    writeDriverPhaseTrace("execute_query",
                                          "send_copy_input_stream",
                                          driverPhaseNowNs() - copy_started,
                                          0,
                                          1,
                                          protocol::MessageType::CopyData,
                                          0,
                                          tls_active_);
                }
                if (status != core::Status::OK) {
                    return status;
                }
                break;
            }
            case protocol::MessageType::CopyOutResponse:
                status = handleCopyOutResponseMessage(msg, results, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                break;
            case protocol::MessageType::CopyBothResponse:
                status = handleCopyBothResponseMessage(msg, results, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                {
                    protocol::CopyInResponse response;
                    status = protocol::parseCopyInResponse(msg.body, response, ctx);
                    if (status != core::Status::OK) {
                        return status;
                    }
                    status = sendCopyInputStream(response, ctx);
                }
                if (status != core::Status::OK) {
                    return status;
                }
                break;
            case protocol::MessageType::CopyData:
                status = handleCopyDataMessage(msg, results, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                break;
            case protocol::MessageType::CopyDone:
                break;
            case protocol::MessageType::CopyFail:
                return handleCopyFailMessage(msg, ctx);
            case protocol::MessageType::CommandComplete: {
                uint8_t command_type = 0;
                uint64_t rows = 0;
                uint64_t last_id = 0;
                std::string tag;
                const int64_t parse_started = phase_trace ? driverPhaseNowNs() : 0;
                status = protocol::parseCommandComplete(msg.body, command_type, rows, last_id, tag, ctx);
                if (phase_trace) {
                    writeDriverPhaseTrace("execute_query",
                                          "parse_command_complete",
                                          driverPhaseNowNs() - parse_started,
                                          msg.body.size(),
                                          rows,
                                          msg.header.type,
                                          msg.header.sequence,
                                          tls_active_);
                }
                if (status != core::Status::OK) {
                    return status;
                }
                results.rows_affected = static_cast<int64_t>(rows);
                results.command_tag = std::move(tag);
                break;
            }
            case protocol::MessageType::Error:
                status = errorAfterStatement(msg, ctx);
                if (copy_hint_sent) {
                    (void)clear_server_copy_hint();
                }
                return status;
            case protocol::MessageType::Ready:
                last_query_sequence_ = 0;
                status = readyAfterStatement(msg.body, ctx);
                if (phase_trace) {
                    writeDriverPhaseTrace("execute_query",
                                          "total",
                                          driverPhaseNowNs() - execute_started,
                                          0,
                                          1,
                                          protocol::MessageType::Ready,
                                          msg.header.sequence,
                                          tls_active_);
                }
                if (status != core::Status::OK || !copy_hint_sent) {
                    return status;
                }
                {
                    const auto clear_status = clear_server_copy_hint();
                    return clear_status == core::Status::OK ? status : clear_status;
                }
            default:
                break;
        }
    }
}

core::Status NetworkClient::prepare(const std::string& sql,
                                    NetworkPreparedStatement& stmt,
                                    core::ErrorContext* ctx) {
    stmt = NetworkPreparedStatement{};
    stmt.sql_ = sql;
    stmt.statement_name_ = "stmt_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    stmt.param_type_oids_ = inferParameterTypesFromSql(sql);
    stmt.params_.resize(stmt.param_type_oids_.size());
    stmt.param_count_ = stmt.param_type_oids_.size();
    stmt.prepared_type_oids_ = stmt.param_type_oids_;
    stmt.owner_session_id_ = session_id_;
    stmt.owner_client_token_ = reinterpret_cast<uintptr_t>(this);

    traceWireEvent("prepare_parse_send", protocol::MessageType::Parse, next_sequence_, in_transaction_);
    auto parse_payload = protocol::buildParsePayload(stmt.statement_name_, sql, stmt.param_type_oids_);
    auto status = sendMessage(protocol::MessageType::Parse, parse_payload, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    traceWireEvent("prepare_describe_send", protocol::MessageType::Describe, next_sequence_, in_transaction_);
    auto describe_payload = protocol::buildDescribePayload(kDescribeStatement, stmt.statement_name_);
    status = sendMessage(protocol::MessageType::Describe, describe_payload, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    traceWireEvent("prepare_sync_send", protocol::MessageType::Sync, next_sequence_, in_transaction_);
    status = sendMessage(protocol::MessageType::Sync, {}, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }

    while (true) {
        protocol::ProtocolMessage msg;
        status = receiveMessage(msg, ctx);
        if (status != core::Status::OK) {
            return status;
        }
        traceWireEvent("prepare_recv", msg.header.type, msg.header.sequence, in_transaction_);
        if (handleAsyncMessage(msg, ctx) == core::Status::OK &&
            isAsyncMessageType(msg.header.type)) {
            continue;
        }
        switch (msg.header.type) {
            case protocol::MessageType::ParameterDescription: {
                std::vector<uint32_t> types;
                status = protocol::parseParameterDescription(msg.body, types, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                stmt.param_count_ = types.size();
                stmt.prepared_type_oids_ = types;
                if (stmt.param_type_oids_.size() < types.size()) {
                    stmt.param_type_oids_.resize(types.size());
                }
                if (stmt.params_.size() < types.size()) {
                    stmt.params_.resize(types.size());
                }
                break;
            }
            case protocol::MessageType::Error:
                return handleErrorResponse(msg, ctx);
            case protocol::MessageType::Ready: {
                status = parseReadyAndTrackTransaction(msg.body, in_transaction_, current_txn_id_, ctx);
                stmt.valid_ = (status == core::Status::OK);
                return status;
            }
            default:
                break;
        }
    }
}

core::Status NetworkClient::executePrepared(NetworkPreparedStatement& stmt,
                                            NetworkResultSet& results,
                                            core::ErrorContext* ctx) {
    if (!stmt.valid_) {
        return setError(ctx, core::Status::INVALID_ARGUMENT, "Prepared statement is not valid");
    }
    if (stmt.owner_client_token_ != reinterpret_cast<uintptr_t>(this) ||
        stmt.owner_session_id_ != session_id_ ||
        isZeroUuidBytes(stmt.owner_session_id_)) {
        return setError(ctx, core::Status::INVALID_ARGUMENT,
                        "Prepared statement is not valid for this client session");
    }
    results = NetworkResultSet{};
    server_autocommit_requested_ = false;

    std::vector<uint32_t> current_types = stmt.param_type_oids_;
    if (current_types.size() < stmt.params_.size()) {
        current_types.resize(stmt.params_.size());
    }
    if (current_types.size() < stmt.param_count_) {
        current_types.resize(stmt.param_count_);
    }
    for (size_t index = 0; index < stmt.params_.size(); ++index) {
        if (current_types[index] == 0 && stmt.params_[index].type_oid != 0) {
            current_types[index] = stmt.params_[index].type_oid;
        }
    }
    if (current_types != stmt.prepared_type_oids_) {
        traceWireEvent("execute_prepared_reparse_send", protocol::MessageType::Parse, next_sequence_, in_transaction_);
        auto parse_payload = protocol::buildParsePayload(stmt.statement_name_, stmt.sql_, current_types);
        auto reparse_status = sendMessage(protocol::MessageType::Parse, parse_payload, 0, false, nullptr, ctx);
        if (reparse_status != core::Status::OK) {
            return reparse_status;
        }
        auto describe_payload = protocol::buildDescribePayload(kDescribeStatement, stmt.statement_name_);
        reparse_status = sendMessage(protocol::MessageType::Describe, describe_payload, 0, false, nullptr, ctx);
        if (reparse_status != core::Status::OK) {
            return reparse_status;
        }
        reparse_status = sendMessage(protocol::MessageType::Sync, {}, 0, false, nullptr, ctx);
        if (reparse_status != core::Status::OK) {
            return reparse_status;
        }
        while (true) {
            protocol::ProtocolMessage msg;
            reparse_status = receiveMessage(msg, ctx);
            if (reparse_status != core::Status::OK) {
                return reparse_status;
            }
            traceWireEvent("execute_prepared_reparse_recv", msg.header.type, msg.header.sequence, in_transaction_);
            if (handleAsyncMessage(msg, ctx) == core::Status::OK &&
                isAsyncMessageType(msg.header.type)) {
                continue;
            }
            if (msg.header.type == protocol::MessageType::ParameterDescription) {
                std::vector<uint32_t> described_types;
                reparse_status = protocol::parseParameterDescription(msg.body, described_types, ctx);
                if (reparse_status != core::Status::OK) {
                    return reparse_status;
                }
                stmt.param_count_ = described_types.size();
                stmt.prepared_type_oids_ = described_types;
                if (stmt.param_type_oids_.size() < described_types.size()) {
                    stmt.param_type_oids_.resize(described_types.size());
                }
                if (stmt.params_.size() < described_types.size()) {
                    stmt.params_.resize(described_types.size());
                }
                continue;
            }
            if (msg.header.type == protocol::MessageType::Error) {
                return handleErrorResponse(msg, ctx);
            }
            if (msg.header.type == protocol::MessageType::Ready) {
                reparse_status = parseReadyAndTrackTransaction(msg.body, in_transaction_, current_txn_id_, ctx);
                if (reparse_status != core::Status::OK) {
                    return reparse_status;
                }
                if (stmt.prepared_type_oids_ != current_types) {
                    stmt.prepared_type_oids_ = current_types;
                }
                break;
            }
        }
    }

    std::string portal_name = "portal_" + std::to_string(next_sequence_);
    traceWireEvent("execute_prepared_bind_send", protocol::MessageType::Bind, next_sequence_, in_transaction_);
    auto bind_payload = protocol::buildBindPayload(portal_name, stmt.statement_name_, stmt.params_, {protocol::kFormatBinary});
    auto status = sendMessage(protocol::MessageType::Bind, bind_payload, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    uint32_t execute_flags = 0;
    if (config_.autocommit && !explicit_transaction_active_ &&
        querySupportsServerAutocommit(stmt.sql_)) {
        execute_flags |= protocol::kExecuteFlagAutocommit;
    }
    auto exec_payload = protocol::buildExecutePayload(portal_name, 0, execute_flags);
    uint32_t seq = 0;
    traceWireEvent("execute_prepared_execute_send", protocol::MessageType::Execute, next_sequence_, in_transaction_);
    status = sendMessage(protocol::MessageType::Execute, exec_payload, 0, false, &seq, ctx);
    if (status != core::Status::OK) {
        server_autocommit_requested_ = false;
        return status;
    }
    last_query_sequence_ = seq;
    server_autocommit_requested_ = (execute_flags & protocol::kExecuteFlagAutocommit) != 0;
    traceWireEvent("execute_prepared_sync_send", protocol::MessageType::Sync, next_sequence_, in_transaction_);
    status = sendMessage(protocol::MessageType::Sync, {}, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        server_autocommit_requested_ = false;
        return status;
    }

    std::vector<protocol::ColumnInfo> cols;
    while (true) {
        protocol::ProtocolMessage msg;
        status = receiveMessage(msg, ctx);
        if (status != core::Status::OK) {
            return status;
        }
        traceWireEvent("execute_prepared_recv", msg.header.type, msg.header.sequence, in_transaction_);
        if (handleAsyncMessage(msg, ctx) == core::Status::OK &&
            isAsyncMessageType(msg.header.type)) {
            continue;
        }
        switch (msg.header.type) {
            case protocol::MessageType::RowDescription: {
                cols.clear();
                status = protocol::parseRowDescription(msg.body, cols, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                results.columns.clear();
                results.columns.reserve(cols.size());
                for (const auto& col : cols) {
                    NetworkColumn out;
                    out.name = col.name;
                    out.type_oid = col.type_oid;
                    out.type_modifier = col.type_modifier;
                    out.format = col.format;
                    out.nullable = col.nullable;
                    results.columns.push_back(std::move(out));
                }
                break;
            }
            case protocol::MessageType::DataRow: {
                std::vector<protocol::ColumnValue> values;
                status = protocol::parseDataRow(msg.body, cols.size(), values, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                results.rows.push_back(std::move(values));
                break;
            }
            case protocol::MessageType::CopyInResponse: {
                protocol::CopyInResponse response;
                status = protocol::parseCopyInResponse(msg.body, response, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                status = sendCopyInputStream(response, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                break;
            }
            case protocol::MessageType::CopyOutResponse:
                status = handleCopyOutResponseMessage(msg, results, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                break;
            case protocol::MessageType::CopyBothResponse:
                status = handleCopyBothResponseMessage(msg, results, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                {
                    protocol::CopyInResponse response;
                    status = protocol::parseCopyInResponse(msg.body, response, ctx);
                    if (status != core::Status::OK) {
                        return status;
                    }
                    status = sendCopyInputStream(response, ctx);
                }
                if (status != core::Status::OK) {
                    return status;
                }
                break;
            case protocol::MessageType::CopyData:
                status = handleCopyDataMessage(msg, results, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                break;
            case protocol::MessageType::CopyDone:
                break;
            case protocol::MessageType::CopyFail:
                return handleCopyFailMessage(msg, ctx);
            case protocol::MessageType::CommandComplete: {
                uint8_t command_type = 0;
                uint64_t rows = 0;
                uint64_t last_id = 0;
                std::string tag;
                status = protocol::parseCommandComplete(msg.body, command_type, rows, last_id, tag, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                results.rows_affected = static_cast<int64_t>(rows);
                results.command_tag = std::move(tag);
                break;
            }
            case protocol::MessageType::Error:
                return errorAfterStatement(msg, ctx);
            case protocol::MessageType::Ready:
                last_query_sequence_ = 0;
                return readyAfterStatement(msg.body, ctx);
            default:
                break;
        }
    }
}

core::Status NetworkClient::prepareServerStatement(const std::string& sql,
                                                   uint32_t& stmt_id,
                                                   core::ErrorContext* ctx) {
    NetworkPreparedStatement stmt;
    auto status = prepare(sql, stmt, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    static uint32_t next_id = 1;
    stmt_id = next_id++;
    prepared_statements_[stmt_id] = std::move(stmt);
    return core::Status::OK;
}

core::Status NetworkClient::executeServerStatement(uint32_t stmt_id,
                                                   const std::vector<protocol::ParamValue>& params,
                                                   NetworkResultSet& results,
                                                   uint32_t max_rows,
                                                   bool /*backward*/,
                                                   bool* portal_suspended_out,
                                                   core::ErrorContext* ctx) {
    auto it = prepared_statements_.find(stmt_id);
    if (it == prepared_statements_.end()) {
        return setError(ctx, core::Status::INVALID_ARGUMENT, "Statement not found");
    }
    results = NetworkResultSet{};
    server_autocommit_requested_ = false;
    if (portal_suspended_out) {
        *portal_suspended_out = false;
    }

    std::string portal_name = "portal_" + std::to_string(next_sequence_);
    auto bind_payload = protocol::buildBindPayload(portal_name, it->second.statement_name_, params, {protocol::kFormatBinary});
    auto status = sendMessage(protocol::MessageType::Bind, bind_payload, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    uint32_t execute_flags = 0;
    if (config_.autocommit && !explicit_transaction_active_ && max_rows == 0 &&
        querySupportsServerAutocommit(it->second.sql_)) {
        execute_flags |= protocol::kExecuteFlagAutocommit;
    }
    auto exec_payload = protocol::buildExecutePayload(portal_name, max_rows, execute_flags);
    uint32_t seq = 0;
    status = sendMessage(protocol::MessageType::Execute, exec_payload, 0, false, &seq, ctx);
    if (status != core::Status::OK) {
        server_autocommit_requested_ = false;
        return status;
    }
    last_query_sequence_ = seq;
    server_autocommit_requested_ = (execute_flags & protocol::kExecuteFlagAutocommit) != 0;
    status = sendMessage(protocol::MessageType::Sync, {}, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        server_autocommit_requested_ = false;
        return status;
    }

    std::vector<protocol::ColumnInfo> cols;
    while (true) {
        protocol::ProtocolMessage msg;
        status = receiveMessage(msg, ctx);
        if (status != core::Status::OK) {
            return status;
        }
        if (handleAsyncMessage(msg, ctx) == core::Status::OK &&
            isAsyncMessageType(msg.header.type)) {
            continue;
        }
        switch (msg.header.type) {
            case protocol::MessageType::RowDescription: {
                cols.clear();
                status = protocol::parseRowDescription(msg.body, cols, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                results.columns.clear();
                results.columns.reserve(cols.size());
                for (const auto& col : cols) {
                    NetworkColumn out;
                    out.name = col.name;
                    out.type_oid = col.type_oid;
                    out.type_modifier = col.type_modifier;
                    out.format = col.format;
                    out.nullable = col.nullable;
                    results.columns.push_back(std::move(out));
                }
                break;
            }
            case protocol::MessageType::DataRow: {
                std::vector<protocol::ColumnValue> values;
                status = protocol::parseDataRow(msg.body, cols.size(), values, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                results.rows.push_back(std::move(values));
                break;
            }
            case protocol::MessageType::CopyInResponse: {
                protocol::CopyInResponse response;
                status = protocol::parseCopyInResponse(msg.body, response, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                status = sendCopyInputStream(response, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                break;
            }
            case protocol::MessageType::CopyOutResponse:
                status = handleCopyOutResponseMessage(msg, results, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                break;
            case protocol::MessageType::CopyBothResponse:
                status = handleCopyBothResponseMessage(msg, results, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                {
                    protocol::CopyInResponse response;
                    status = protocol::parseCopyInResponse(msg.body, response, ctx);
                    if (status != core::Status::OK) {
                        return status;
                    }
                    status = sendCopyInputStream(response, ctx);
                }
                if (status != core::Status::OK) {
                    return status;
                }
                break;
            case protocol::MessageType::CopyData:
                status = handleCopyDataMessage(msg, results, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                break;
            case protocol::MessageType::CopyDone:
                break;
            case protocol::MessageType::CopyFail:
                return handleCopyFailMessage(msg, ctx);
            case protocol::MessageType::CommandComplete: {
                uint8_t command_type = 0;
                uint64_t rows = 0;
                uint64_t last_id = 0;
                std::string tag;
                status = protocol::parseCommandComplete(msg.body, command_type, rows, last_id, tag, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                results.rows_affected = static_cast<int64_t>(rows);
                results.command_tag = std::move(tag);
                break;
            }
            case protocol::MessageType::PortalSuspended:
                if (portal_suspended_out) {
                    *portal_suspended_out = true;
                }
                last_query_sequence_ = 0;
                return drainUntilReady(nullptr, nullptr, nullptr, ctx);
            case protocol::MessageType::Error:
                return errorAfterStatement(msg, ctx);
            case protocol::MessageType::Ready:
                last_query_sequence_ = 0;
                return readyAfterStatement(msg.body, ctx);
            default:
                break;
        }
    }
}

core::Status NetworkClient::closeServerStatement(uint32_t stmt_id,
                                                 core::ErrorContext* ctx) {
    auto it = prepared_statements_.find(stmt_id);
    if (it == prepared_statements_.end()) {
        return setError(ctx, core::Status::INVALID_ARGUMENT, "Statement not found");
    }
    auto close_payload = protocol::buildClosePayload(kCloseStatement, it->second.statement_name_);
    auto status = sendMessage(protocol::MessageType::Close, close_payload, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    status = sendMessage(protocol::MessageType::Sync, {}, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    prepared_statements_.erase(it);
    std::string tag;
    uint64_t rows = 0;
    uint64_t last_id = 0;
    return drainUntilReady(&tag, &rows, &last_id, ctx);
}

core::Status NetworkClient::sendQueryCancel(core::ErrorContext* ctx) {
    if (!connected_) {
        return setError(ctx, core::Status::CONNECTION_DOES_NOT_EXIST, "Connection not open");
    }
    if (last_query_sequence_ == 0) {
        return setError(ctx, core::Status::INVALID_ARGUMENT, "No in-flight query to cancel");
    }
    auto payload = protocol::buildCancelPayload(kCancelTypeStatement, last_query_sequence_);
    return sendMessage(protocol::MessageType::Cancel, payload, protocol::kFlagUrgent, false, nullptr, ctx);
}

core::Status NetworkClient::subscribeNotifications(uint8_t subscribe_type,
                                                   const std::string& channel,
                                                   const std::string& filter,
                                                   core::ErrorContext* ctx) {
    auto payload = protocol::buildSubscribePayload(subscribe_type, channel, filter);
    auto status = sendMessage(protocol::MessageType::Subscribe, payload, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    return drainUntilReady(nullptr, nullptr, nullptr, ctx);
}

core::Status NetworkClient::unsubscribeNotifications(const std::string& channel,
                                                     core::ErrorContext* ctx) {
    auto payload = protocol::buildUnsubscribePayload(channel);
    auto status = sendMessage(protocol::MessageType::Unsubscribe, payload, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    return drainUntilReady(nullptr, nullptr, nullptr, ctx);
}

core::Status NetworkClient::streamControl(uint8_t control_type,
                                          uint32_t window_size,
                                          uint32_t timeout_ms,
                                          core::ErrorContext* ctx) {
    auto payload = protocol::buildStreamControlPayload(control_type, window_size, timeout_ms);
    return sendMessage(protocol::MessageType::StreamControl, payload, 0, false, nullptr, ctx);
}

core::Status NetworkClient::attachCreate(const std::string& mode,
                                         const std::string& db_name,
                                         core::ErrorContext* ctx) {
    auto payload = protocol::buildAttachCreatePayload(mode, db_name);
    auto status = sendMessage(protocol::MessageType::AttachCreate, payload, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    return drainUntilReady(nullptr, nullptr, nullptr, ctx);
}

core::Status NetworkClient::attachDetach(core::ErrorContext* ctx) {
    auto payload = protocol::buildAttachDetachPayload();
    auto status = sendMessage(protocol::MessageType::AttachDetach, payload, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    return drainUntilReady(nullptr, nullptr, nullptr, ctx);
}

core::Status NetworkClient::attachList(NetworkResultSet& results,
                                       core::ErrorContext* ctx) {
    results = NetworkResultSet{};
    auto payload = protocol::buildAttachListPayload();
    auto status = sendMessage(protocol::MessageType::AttachList, payload, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    status = sendMessage(protocol::MessageType::Sync, {}, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    std::vector<protocol::ColumnInfo> cols;
    while (true) {
        protocol::ProtocolMessage msg;
        status = receiveMessage(msg, ctx);
        if (status != core::Status::OK) {
            return status;
        }
        if (handleAsyncMessage(msg, ctx) == core::Status::OK &&
            isAsyncMessageType(msg.header.type)) {
            continue;
        }
        switch (msg.header.type) {
            case protocol::MessageType::RowDescription: {
                cols.clear();
                status = protocol::parseRowDescription(msg.body, cols, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                results.columns.clear();
                results.columns.reserve(cols.size());
                for (const auto& col : cols) {
                    NetworkColumn out;
                    out.name = col.name;
                    out.type_oid = col.type_oid;
                    out.type_modifier = col.type_modifier;
                    out.format = col.format;
                    out.nullable = col.nullable;
                    results.columns.push_back(std::move(out));
                }
                break;
            }
            case protocol::MessageType::DataRow: {
                std::vector<protocol::ColumnValue> values;
                status = protocol::parseDataRow(msg.body, cols.size(), values, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                results.rows.push_back(std::move(values));
                break;
            }
            case protocol::MessageType::CommandComplete: {
                uint8_t command_type = 0;
                uint64_t rows = 0;
                uint64_t last_id = 0;
                std::string tag;
                status = protocol::parseCommandComplete(msg.body, command_type, rows, last_id, tag, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                results.rows_affected = static_cast<int64_t>(rows);
                results.command_tag = std::move(tag);
                break;
            }
            case protocol::MessageType::Error:
                return handleErrorResponse(msg, ctx);
            case protocol::MessageType::Ready:
                return parseReadyAndTrackTransaction(msg.body, in_transaction_, current_txn_id_, ctx);
            default:
                break;
        }
    }
}

core::Status NetworkClient::setOption(const std::string& name,
                                      const std::string& value,
                                      core::ErrorContext* ctx) {
    auto payload = protocol::buildSetOptionPayload(name, value);
    auto status = sendMessage(protocol::MessageType::SetOption, payload, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    return drainUntilReady(nullptr, nullptr, nullptr, ctx);
}

core::Status NetworkClient::ping(core::ErrorContext* ctx) {
    auto status = sendMessage(protocol::MessageType::Ping, {}, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    while (true) {
        protocol::ProtocolMessage msg;
        status = receiveMessage(msg, ctx);
        if (status != core::Status::OK) {
            return status;
        }
        if (handleAsyncMessage(msg, ctx) == core::Status::OK &&
            isAsyncMessageType(msg.header.type)) {
            continue;
        }
        switch (msg.header.type) {
            case protocol::MessageType::Pong:
                return core::Status::OK;
            case protocol::MessageType::Ready:
                return parseReadyAndTrackTransaction(msg.body, in_transaction_, current_txn_id_, ctx);
            case protocol::MessageType::Error:
                return handleErrorResponse(msg, ctx);
            default:
                break;
        }
    }
}

core::Status NetworkClient::executeSblr(uint64_t sblr_hash,
                                        const std::vector<uint8_t>& bytecode,
                                        const std::vector<protocol::ParamValue>& params,
                                        NetworkResultSet& results,
                                        core::ErrorContext* ctx) {
    results = NetworkResultSet{};
    server_autocommit_requested_ = false;
    auto payload = protocol::buildSblrExecutePayload(sblr_hash, bytecode, params);
    uint32_t seq = 0;
    auto status = sendMessage(protocol::MessageType::SblrExecute, payload, 0, false, &seq, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    last_query_sequence_ = seq;

    std::vector<protocol::ColumnInfo> cols;
    while (true) {
        protocol::ProtocolMessage msg;
        status = receiveMessage(msg, ctx);
        if (status != core::Status::OK) {
            return status;
        }
        if (handleAsyncMessage(msg, ctx) == core::Status::OK &&
            isAsyncMessageType(msg.header.type)) {
            continue;
        }
        switch (msg.header.type) {
            case protocol::MessageType::RowDescription: {
                cols.clear();
                status = protocol::parseRowDescription(msg.body, cols, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                results.columns.clear();
                results.columns.reserve(cols.size());
                for (const auto& col : cols) {
                    NetworkColumn out;
                    out.name = col.name;
                    out.type_oid = col.type_oid;
                    out.type_modifier = col.type_modifier;
                    out.format = col.format;
                    out.nullable = col.nullable;
                    results.columns.push_back(std::move(out));
                }
                break;
            }
            case protocol::MessageType::DataRow: {
                std::vector<protocol::ColumnValue> values;
                status = protocol::parseDataRow(msg.body, cols.size(), values, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                results.rows.push_back(std::move(values));
                break;
            }
            case protocol::MessageType::CommandComplete: {
                uint8_t command_type = 0;
                uint64_t rows = 0;
                uint64_t last_id = 0;
                std::string tag;
                status = protocol::parseCommandComplete(msg.body, command_type, rows, last_id, tag, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                results.rows_affected = static_cast<int64_t>(rows);
                results.command_tag = std::move(tag);
                break;
            }
            case protocol::MessageType::Error:
                return errorAfterStatement(msg, ctx);
            case protocol::MessageType::Ready:
                last_query_sequence_ = 0;
                return readyAfterStatement(msg.body, ctx);
            default:
                break;
        }
    }
}

void NetworkClient::resetQueryProgress() {
    query_progress_ = QueryProgressSnapshot{};
}

void NetworkClient::drainNotifications(std::vector<Notification>& out) {
    out = std::move(notifications_);
    notifications_.clear();
}

bool NetworkClient::takeLastQueryPlan(protocol::QueryPlan& out) {
    if (!last_plan_) {
        return false;
    }
    out = std::move(*last_plan_);
    last_plan_.reset();
    return true;
}

bool NetworkClient::takeLastSblrCompiled(protocol::SblrCompiled& out) {
    if (!last_sblr_) {
        return false;
    }
    out = std::move(*last_sblr_);
    last_sblr_.reset();
    return true;
}

core::Status NetworkClient::beginTransaction(const TransactionOptions& options,
                                             core::ErrorContext* ctx) {
    if (in_transaction_) {
        if (canAdoptFreshNativeBoundary(options)) {
            // Native MGA sessions are always inside an engine-owned
            // transaction. A default begin adopts the current boundary.
            explicit_transaction_active_ = true;
            return core::Status::OK;
        }
        // Optioned begin is a request to validate/apply transaction
        // characteristics at the active MGA boundary; the server owns whether
        // that adopts, replaces, or rejects the boundary.
    }

    uint16_t flags = options.flags;
    uint8_t isolation_level = options.isolation_level;
    if (options.read_committed_mode != 0) {
        flags |= 0x0100;
        if ((flags & 0x0001) == 0) {
            flags |= 0x0001;
            isolation_level = 1;
        }
    }

    auto payload = protocol::buildTxnBeginPayload(flags,
                                                  options.conflict_action,
                                                  options.autocommit_mode,
                                                  isolation_level,
                                                  options.access_mode,
                                                  options.deferrable,
                                                  options.wait_mode,
                                                  options.timeout_ms,
                                                  options.read_committed_mode);
    auto status = sendMessage(protocol::MessageType::TxnBegin, payload, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    status = drainUntilReady(nullptr, nullptr, nullptr, ctx);
    if (status == core::Status::OK) {
        explicit_transaction_active_ = true;
    }
    return status;
}

core::Status NetworkClient::beginTransaction(core::ErrorContext* ctx) {
    return beginTransaction(TransactionOptions{}, ctx);
}

core::Status NetworkClient::commit(core::ErrorContext* ctx) {
    traceWireEvent("commit_send",
                   protocol::MessageType::TxnCommit,
                   next_sequence_,
                   in_transaction_);
    protocol::TxnCommitRequest request;
    request.contract_flags = protocol::kTxnCommitFlagHasIdempotencyKey |
                             protocol::kTxnCommitFlagStatementHasSideEffects;
    request.idempotency_key = randomProtocolUuid();
    request.request_fingerprint = fnv1a64("transaction.commit");
    request.expected_txn_id = current_txn_id_;
    last_commit_finality_ = NetworkTransactionFinality{};
    last_commit_finality_.state = protocol::TxnFinalityState::Unknown;
    last_commit_finality_.idempotency_key = request.idempotency_key;
    last_commit_finality_.request_fingerprint = request.request_fingerprint;
    last_commit_finality_.original_txn_id = current_txn_id_;
    last_commit_finality_.diagnostic_code = "SBWP.COMMIT.FINALITY_PENDING";
    last_commit_finality_.detail = "commit_outcome_pending_engine_finality_report";
    last_commit_finality_present_ = true;

    auto payload = protocol::buildTxnCommitPayload(request);
    auto status = sendMessage(protocol::MessageType::TxnCommit, payload, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        last_commit_finality_.diagnostic_code = "SBWP.COMMIT.SEND_FAILED_FINALITY_UNKNOWN";
        last_commit_finality_.detail = "commit_send_failed_before_engine_finality_report";
        return status;
    }
    std::string command_tag;
    status = drainUntilReady(&command_tag, nullptr, nullptr, ctx);
    if (status != core::Status::OK) {
        last_commit_finality_.state = protocol::TxnFinalityState::Unknown;
        last_commit_finality_.flags &= static_cast<uint16_t>(~protocol::kTxnFinalityFlagEngineKnown);
        last_commit_finality_.diagnostic_code = "SBWP.COMMIT.READY_LOST_FINALITY_UNKNOWN";
        last_commit_finality_.detail = "commit_outcome_unknown_until_engine_finality_query";
        return status;
    }
    if (status == core::Status::OK) {
        if (last_commit_finality_.state == protocol::TxnFinalityState::Unknown &&
            command_tag == "COMMIT" &&
            last_commit_finality_.diagnostic_code == "SBWP.COMMIT.FINALITY_PENDING") {
            last_commit_finality_.state = protocol::TxnFinalityState::Committed;
            last_commit_finality_.flags = protocol::kTxnFinalityFlagEngineKnown |
                                          protocol::kTxnFinalityFlagSameIdempotencyKeyReplayable;
            last_commit_finality_.replacement_txn_id = current_txn_id_;
            last_commit_finality_.diagnostic_code = "SBWP.COMMIT.READY_COMMITTED";
            last_commit_finality_.detail = "legacy_ready_after_commit_reported_replacement_boundary";
        }
        explicit_transaction_active_ = false;
    }
    return status;
}

core::Status NetworkClient::queryLastCommitFinality(core::ErrorContext* ctx) {
    NetworkTransactionFinality queried;
    auto status = queryCommitFinality(last_commit_finality_.idempotency_key,
                                      last_commit_finality_.finality_token,
                                      queried,
                                      ctx);
    if (status == core::Status::OK) {
        last_commit_finality_ = queried;
        last_commit_finality_present_ = true;
    }
    return status;
}

core::Status NetworkClient::queryCommitFinality(
    const std::array<uint8_t, 16>& idempotency_key,
    const std::array<uint8_t, 16>& finality_token,
    NetworkTransactionFinality& finality,
    core::ErrorContext* ctx) {
    protocol::TxnFinalityQuery query;
    query.idempotency_key = idempotency_key;
    query.finality_token = finality_token;
    query.expected_txn_id = current_txn_id_;
    const auto payload = protocol::buildTxnFinalityQueryPayload(query);
    auto status = sendMessage(protocol::MessageType::TxnStatus, payload, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    bool saw_status = false;
    NetworkTransactionFinality observed;
    while (true) {
        protocol::ProtocolMessage msg;
        status = receiveMessage(msg, ctx);
        if (status != core::Status::OK) {
            return status;
        }
        if (handleAsyncMessage(msg, ctx) == core::Status::OK &&
            isAsyncMessageType(msg.header.type)) {
            continue;
        }
        switch (msg.header.type) {
            case protocol::MessageType::TxnStatus: {
                status = handleTxnStatusMessage(msg, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                observed = last_commit_finality_;
                saw_status = true;
                break;
            }
            case protocol::MessageType::Ready: {
                auto ready_status =
                    parseReadyAndTrackTransaction(msg.body, in_transaction_, current_txn_id_, ctx);
                if (ready_status != core::Status::OK) {
                    return ready_status;
                }
                if (!saw_status) {
                    return setError(ctx,
                                    core::Status::PROTOCOL_VIOLATION,
                                    "TxnStatus query completed without a finality status");
                }
                finality = observed;
                return core::Status::OK;
            }
            case protocol::MessageType::Error:
                return handleErrorResponse(msg, ctx);
            default:
                break;
        }
    }
}

core::Status NetworkClient::validateRetryAfterCommitUncertainty(
    const std::array<uint8_t, 16>& idempotency_key,
    bool statement_has_side_effects,
    bool caller_acknowledged_retry_boundary,
    core::ErrorContext* ctx) const {
    if (!last_commit_finality_present_) {
        return setError(ctx,
                        core::Status::INVALID_TRANSACTION_STATE,
                        "No commit finality record is available for retry validation");
    }
    if (!last_commit_finality_.engineFinalityKnown()) {
        const auto status =
            setError(ctx,
                     core::Status::CONNECTION_FAILURE,
                     "Retry refused until engine MGA finality is queried");
        if (ctx) {
            ctx->setSQLState("08007");
        }
        return status;
    }
    if (!uuidBytesEqual(idempotency_key, last_commit_finality_.idempotency_key)) {
        const auto status =
            setError(ctx,
                     core::Status::INVALID_TRANSACTION_STATE,
                     "Retry idempotency key differs from the recorded commit key");
        if (ctx) {
            ctx->setSQLState("40003");
        }
        return status;
    }
    if (statement_has_side_effects && !caller_acknowledged_retry_boundary) {
        const auto status =
            setError(ctx,
                     core::Status::INVALID_TRANSACTION_STATE,
                     "Side-effecting retry requires caller acknowledgement");
        if (ctx) {
            ctx->setSQLState("40003");
        }
        return status;
    }
    return core::Status::OK;
}

core::Status NetworkClient::rollback(core::ErrorContext* ctx) {
    auto payload = protocol::buildTxnRollbackPayload(0);
    auto status = sendMessage(protocol::MessageType::TxnRollback, payload, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    status = drainUntilReady(nullptr, nullptr, nullptr, ctx);
    if (status == core::Status::OK) {
        explicit_transaction_active_ = false;
    }
    return status;
}

core::Status NetworkClient::savepoint(const std::string& name,
                                      core::ErrorContext* ctx) {
    if (name.empty()) {
        return setError(ctx, core::Status::INVALID_ARGUMENT, "Savepoint name is required");
    }
    auto payload = protocol::buildTxnSavepointPayload(name);
    auto status = sendMessage(protocol::MessageType::TxnSavepoint, payload, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    return drainUntilReady(nullptr, nullptr, nullptr, ctx);
}

core::Status NetworkClient::releaseSavepoint(const std::string& name,
                                             core::ErrorContext* ctx) {
    if (name.empty()) {
        return setError(ctx, core::Status::INVALID_ARGUMENT, "Savepoint name is required");
    }
    auto payload = protocol::buildTxnReleasePayload(name);
    auto status = sendMessage(protocol::MessageType::TxnRelease, payload, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    return drainUntilReady(nullptr, nullptr, nullptr, ctx);
}

core::Status NetworkClient::rollbackToSavepoint(const std::string& name,
                                                core::ErrorContext* ctx) {
    if (name.empty()) {
        return setError(ctx, core::Status::INVALID_ARGUMENT, "Savepoint name is required");
    }
    auto payload = protocol::buildTxnRollbackToPayload(name);
    auto status = sendMessage(protocol::MessageType::TxnRollbackTo, payload, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    return drainUntilReady(nullptr, nullptr, nullptr, ctx);
}

core::Status NetworkClient::sendManagerFrame(uint8_t type,
                                             const std::vector<uint8_t>& payload,
                                             core::ErrorContext* ctx) {
    std::vector<uint8_t> frame;
    frame.reserve(kManagerHeaderSize + payload.size());
    appendU32(frame, kManagerProtocolMagic);
    appendU16(frame, kManagerProtocolVersion);
    frame.push_back(type);
    frame.push_back(0);  // flags
    appendU32(frame, static_cast<uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());

    if (tls_active_) {
        size_t total = 0;
        while (total < frame.size()) {
            int rc = tls_conn_->write(frame.data() + total, static_cast<int>(frame.size() - total));
            if (rc <= 0) {
                return setError(ctx, core::Status::CONNECTION_FAILURE, "Manager frame write failed");
            }
            total += static_cast<size_t>(rc);
        }
        return core::Status::OK;
    }

    auto status = socket_->writeExact(frame.data(), frame.size(), ctx);
    if (status != core::Status::OK) {
        return setError(ctx, core::Status::CONNECTION_FAILURE, "Manager frame write failed");
    }
    return core::Status::OK;
}

core::Status NetworkClient::receiveManagerFrame(uint8_t& type,
                                                std::vector<uint8_t>& payload,
                                                core::ErrorContext* ctx) {
    uint8_t header[kManagerHeaderSize] = {0};
    auto status = readExactWithTimeout(header, sizeof(header), ctx);
    if (status != core::Status::OK) {
        return status;
    }

    const uint32_t magic = readU32(header);
    if (magic != kManagerProtocolMagic) {
        return setError(ctx, core::Status::PROTOCOL_VIOLATION, "Manager frame magic mismatch");
    }
    const uint16_t version = readU16(header + 4);
    if (version != kManagerProtocolVersion) {
        return setError(ctx, core::Status::PROTOCOL_VIOLATION, "Manager frame version mismatch");
    }
    type = header[6];
    const uint32_t payload_len = readU32(header + 8);
    if (payload_len > kManagerMaxPayloadSize) {
        return setError(ctx, core::Status::PROTOCOL_VIOLATION, "Manager payload too large");
    }

    payload.clear();
    if (payload_len == 0) {
        return core::Status::OK;
    }
    payload.resize(payload_len);
    return readExactWithTimeout(payload.data(), payload.size(), ctx);
}

core::Status NetworkClient::performManagerConnect(core::ErrorContext* ctx) {
    if (config_.manager_auth_token.empty()) {
        return setError(ctx, core::Status::INVALID_ARGUMENT,
                        "manager_proxy mode requires manager_auth_token");
    }

    const std::string manager_user =
        config_.manager_username.empty() ? (config_.username.empty() ? "admin" : config_.username)
                                         : config_.manager_username;
    const std::string manager_db =
        config_.manager_database.empty() ? config_.database : config_.manager_database;
    const std::string manager_profile =
        config_.manager_connection_profile.empty() ? "SBsql" : config_.manager_connection_profile;
    const std::string manager_intent =
        config_.manager_client_intent.empty() ? "SBsql" : config_.manager_client_intent;

    // MCP_HELLO
    std::vector<uint8_t> hello_payload;
    hello_payload.reserve(4);
    appendU16(hello_payload, kMcpProtocolVersion);
    appendU16(hello_payload, config_.manager_client_flags);
    auto status = sendManagerFrame(kMsgMcpHello, hello_payload, ctx);
    if (status != core::Status::OK) {
        return status;
    }

    uint8_t msg_type = 0;
    std::vector<uint8_t> msg_payload;
    status = receiveManagerFrame(msg_type, msg_payload, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    if (msg_type != kMsgStatusResponse) {
        return setError(ctx, core::Status::PROTOCOL_VIOLATION, "Expected MCP hello status response");
    }

    // MCP_AUTH_START
    std::vector<uint8_t> auth_start;
    appendLengthPrefixedString(auth_start, manager_user);
    auth_start.push_back(kMcpAuthMethodToken);
    if (config_.manager_auth_fast_path) {
        appendU32(auth_start, static_cast<uint32_t>(config_.manager_auth_token.size()));
        auth_start.insert(auth_start.end(),
                          config_.manager_auth_token.begin(),
                          config_.manager_auth_token.end());
    } else {
        appendU32(auth_start, 0);
    }
    status = sendManagerFrame(kMsgMcpAuthStart, auth_start, ctx);
    if (status != core::Status::OK) {
        return status;
    }

    status = receiveManagerFrame(msg_type, msg_payload, ctx);
    if (status != core::Status::OK) {
        return status;
    }

    auto parseAuthResponseStatus = [&](const std::vector<uint8_t>& payload,
                                       std::string* error_out) -> core::Status {
        if (payload.size() < 1 + 4 + 256) {
            return setError(ctx, core::Status::PROTOCOL_VIOLATION, "Truncated MCP auth response");
        }
        const uint8_t auth_status = payload[0];
        if (auth_status == 0) {
            return core::Status::OK;
        }
        std::string err(reinterpret_cast<const char*>(payload.data() + 5), 256);
        const auto nul = err.find('\0');
        if (nul != std::string::npos) {
            err.resize(nul);
        }
        if (error_out) {
            *error_out = err;
        }
        return core::Status::INVALID_AUTHORIZATION;
    };

    if (msg_type == kMsgAuthChallenge) {
        std::vector<uint8_t> auth_continue;
        appendU32(auth_continue, static_cast<uint32_t>(config_.manager_auth_token.size()));
        auth_continue.insert(auth_continue.end(),
                             config_.manager_auth_token.begin(),
                             config_.manager_auth_token.end());
        status = sendManagerFrame(kMsgMcpAuthContinue, auth_continue, ctx);
        if (status != core::Status::OK) {
            return status;
        }
        status = receiveManagerFrame(msg_type, msg_payload, ctx);
        if (status != core::Status::OK) {
            return status;
        }
    }

    if (msg_type != kMsgAuthResponse) {
        return setError(ctx, core::Status::PROTOCOL_VIOLATION, "Expected MCP auth response");
    }
    std::string auth_error;
    status = parseAuthResponseStatus(msg_payload, &auth_error);
    if (status != core::Status::OK) {
        return setError(ctx,
                        core::Status::INVALID_AUTHORIZATION,
                        auth_error.empty() ? "MCP authentication failed" : auth_error);
    }
    resolved_auth_context_.manager_authenticated = true;

    // MCP_DB_CONNECT (extended payload)
    std::vector<uint8_t> db_connect;
    db_connect.push_back('M');
    db_connect.push_back('C');
    db_connect.push_back('P');
    db_connect.push_back('1');
    appendLengthPrefixedString(db_connect, manager_db);
    appendLengthPrefixedString(db_connect, manager_profile);
    appendLengthPrefixedString(db_connect, manager_intent);
    std::vector<uint8_t> nonce(16);
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        return setError(ctx, core::Status::INTERNAL_ERROR, "Failed to generate MCP nonce");
    }
    appendU16(db_connect, static_cast<uint16_t>(nonce.size()));
    db_connect.insert(db_connect.end(), nonce.begin(), nonce.end());
    status = sendManagerFrame(kMsgMcpDbConnect, db_connect, ctx);
    if (status != core::Status::OK) {
        return status;
    }

    status = receiveManagerFrame(msg_type, msg_payload, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    if (msg_type != kMsgConnectResponse) {
        return setError(ctx, core::Status::PROTOCOL_VIOLATION, "Expected MCP connect response");
    }
    if (msg_payload.size() < 1 + 2 + 2 + 16 + 64 + 32) {
        return setError(ctx, core::Status::PROTOCOL_VIOLATION, "Truncated MCP connect response");
    }
    const bool connect_ok = (msg_payload[0] == 0);
    if (!connect_ok) {
        std::string connect_error = "MCP database connect failed";
        const size_t err_offset = 1 + 2 + 2 + 16 + 64 + 32;
        if (msg_payload.size() >= err_offset + 4) {
            const uint32_t err_len = readU32(msg_payload.data() + err_offset);
            if (err_offset + 4 + err_len <= msg_payload.size()) {
                connect_error.assign(
                    reinterpret_cast<const char*>(msg_payload.data() + err_offset + 4),
                    err_len);
            }
        }
        return setError(ctx, core::Status::INVALID_AUTHORIZATION, connect_error);
    }

    return core::Status::OK;
}

core::Status NetworkClient::sendMessage(const protocol::ProtocolMessage& msg,
                                        core::ErrorContext* ctx) {
    auto buf = protocol::encodeMessage(msg.header, msg.body);
    if (tls_active_) {
        size_t total = 0;
        while (total < buf.size()) {
            if (socket_ && !socket_->waitWritable(static_cast<int>(config_.write_timeout_ms))) {
                return setError(ctx, core::Status::IO_ERROR, "TLS write timeout");
            }
            int rc = tls_conn_->write(buf.data() + total, static_cast<int>(buf.size() - total));
            if (rc <= 0) {
                return setError(ctx, core::Status::CONNECTION_FAILURE, "TLS write failed");
            }
            total += static_cast<size_t>(rc);
        }
        return core::Status::OK;
    }
    size_t written = 0;
    auto status = socket_->writeExact(buf.data(), buf.size(), ctx);
    if (status != core::Status::OK) {
        return setError(ctx, core::Status::CONNECTION_FAILURE, "Socket write failed");
    }
    return core::Status::OK;
}

core::Status NetworkClient::sendRawPayloadMessage(protocol::MessageType type,
                                                  const uint8_t* payload,
                                                  size_t payload_size,
                                                  uint8_t flags,
                                                  bool force_zero,
                                                  core::ErrorContext* ctx) {
    if (payload_size > protocol::kMaxMessageSize) {
        return setError(ctx, core::Status::PROTOCOL_VIOLATION, "Message too large");
    }

    std::array<uint8_t, protocol::kHeaderSize> header{};
    storeU32(header.data(), protocol::kProtocolMagic);
    header[4] = protocol::kProtocolMajor;
    header[5] = protocol::kProtocolMinor;
    header[6] = static_cast<uint8_t>(type);
    header[7] = flags;
    storeU32(header.data() + 8, static_cast<uint32_t>(payload_size));
    const uint32_t sequence = force_zero ? 0 : next_sequence_++;
    storeU32(header.data() + 12, sequence);
    if (!force_zero) {
        std::memcpy(header.data() + 16, session_id_.data(), session_id_.size());
        storeU64(header.data() + 32, current_txn_id_);
    }

    auto write_bytes = [&](const uint8_t* data, size_t size) -> core::Status {
        size_t total = 0;
        while (total < size) {
            if (tls_active_) {
                if (socket_ && !socket_->waitWritable(static_cast<int>(config_.write_timeout_ms))) {
                    return setError(ctx, core::Status::IO_ERROR, "TLS write timeout");
                }
                const size_t remaining = size - total;
                const int write_size = static_cast<int>(
                    std::min<size_t>(remaining, static_cast<size_t>(std::numeric_limits<int>::max())));
                const int rc = tls_conn_->write(data + total, write_size);
                if (rc <= 0) {
                    return setError(ctx, core::Status::CONNECTION_FAILURE, "TLS write failed");
                }
                total += static_cast<size_t>(rc);
            } else {
                const auto status = socket_->writeExact(data + total, size - total, ctx);
                if (status != core::Status::OK) {
                    return setError(ctx, core::Status::CONNECTION_FAILURE, "Socket write failed");
                }
                total = size;
            }
        }
        return core::Status::OK;
    };

    auto status = write_bytes(header.data(), header.size());
    if (status != core::Status::OK || payload_size == 0) {
        return status;
    }
    return write_bytes(payload, payload_size);
}

core::Status NetworkClient::receiveMessage(protocol::ProtocolMessage& msg,
                                           core::ErrorContext* ctx) {
    std::vector<uint8_t> header_bytes(protocol::kHeaderSize);
    auto status = readExactWithTimeout(header_bytes.data(), header_bytes.size(), ctx);
    if (status != core::Status::OK) {
        return status;
    }
    protocol::MessageHeader header;
    status = protocol::decodeHeader(header_bytes, header, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    msg.header = header;
    msg.body.clear();
    if (header.length > 0) {
        msg.body.resize(header.length);
        status = readExactWithTimeout(msg.body.data(), msg.body.size(), ctx);
        if (status != core::Status::OK) {
            return status;
        }
    }
    return core::Status::OK;
}

core::Status NetworkClient::readExactWithTimeout(void* buffer, size_t size,
                                                 core::ErrorContext* ctx) {
    if (size == 0) {
        return core::Status::OK;
    }
    size_t total = 0;
    while (total < size) {
        if (tls_active_) {
            if (tls_conn_->pending() <= 0 &&
                socket_ &&
                !socket_->waitReadable(static_cast<int>(config_.read_timeout_ms))) {
                return setError(ctx, core::Status::IO_ERROR, "TLS read timeout");
            }
            int rc = tls_conn_->read(static_cast<uint8_t*>(buffer) + total,
                                     static_cast<int>(size - total));
            if (rc <= 0) {
                return setError(ctx, core::Status::CONNECTION_FAILURE, "TLS read failed");
            }
            total += static_cast<size_t>(rc);
            continue;
        }
        size_t bytes_read = 0;
        auto status = socket_->readWithTimeout(static_cast<uint8_t*>(buffer) + total,
                                               size - total,
                                               &bytes_read,
                                               config_.read_timeout_ms,
                                               ctx);
        if (status != core::Status::OK) {
            return status;
        }
        if (bytes_read == 0) {
            return setError(ctx, core::Status::CONNECTION_FAILURE, "Connection closed");
        }
        total += bytes_read;
    }
    return core::Status::OK;
}

core::Status NetworkClient::sendCopyInputStream(const protocol::CopyInResponse& response,
                                                core::ErrorContext* ctx) {
    if (!copy_input_stream_) {
        const std::string message = "COPY FROM STDIN requires a client input stream";
        const auto fail_payload = protocol::buildCopyFailPayload(message);
        auto fail_status = sendMessage(protocol::MessageType::CopyFail, fail_payload, 0, false, nullptr, ctx);
        if (fail_status != core::Status::OK) {
            return fail_status;
        }
        return setError(ctx, core::Status::INVALID_ARGUMENT, message);
    }

    const bool phase_trace = driverPhaseTraceEnabled();
    const int64_t total_started = phase_trace ? driverPhaseNowNs() : 0;
    int64_t read_elapsed_ns = 0;
    int64_t send_data_elapsed_ns = 0;
    size_t total_read = 0;
    size_t total_sent = 0;
    size_t chunk_count = 0;
    const size_t chunk_size =
        std::max<uint32_t>(1, response.window_bytes == 0 ? config_.copy_chunk_bytes
                                                         : response.window_bytes);
    const bool binary_copy = response.format == protocol::kFormatBinary;
    std::vector<uint8_t> buffer(chunk_size);
    std::string pending;
    auto send_payload = [&](const uint8_t* data, size_t size) -> core::Status {
        core::Status status = core::Status::OK;
        const int64_t send_started = phase_trace ? driverPhaseNowNs() : 0;
        if (tls_active_) {
            std::vector<uint8_t> chunk(data, data + size);
            status = sendMessage(protocol::MessageType::CopyData,
                                 chunk,
                                 0,
                                 false,
                                 nullptr,
                                 ctx);
        } else {
            status = sendRawPayloadMessage(protocol::MessageType::CopyData,
                                           data,
                                           size,
                                           0,
                                           false,
                                           ctx);
        }
        if (phase_trace) {
            send_data_elapsed_ns += driverPhaseNowNs() - send_started;
        }
        if (status != core::Status::OK) {
            return status;
        }
        total_sent += size;
        ++chunk_count;
        return core::Status::OK;
    };
    auto send_copy_fail = [&](const std::string& message,
                              core::Status status_code) -> core::Status {
        const auto fail_payload = protocol::buildCopyFailPayload(message);
        auto fail_status = sendMessage(protocol::MessageType::CopyFail,
                                       fail_payload,
                                       0,
                                       false,
                                       nullptr,
                                       ctx);
        if (fail_status != core::Status::OK) {
            return fail_status;
        }
        return setError(ctx, status_code, message);
    };
    std::vector<BinaryCopyRow> binary_rows;
    size_t binary_payload_bytes = 12;
    auto flush_binary_rows = [&](bool force) -> core::Status {
        if (binary_rows.empty()) {
            return core::Status::OK;
        }
        if (!force && binary_payload_bytes < chunk_size) {
            return core::Status::OK;
        }
        const auto payload = buildBinaryCopyFrame(binary_rows);
        auto status = send_payload(payload.data(), payload.size());
        if (status != core::Status::OK) {
            return status;
        }
        binary_rows.clear();
        binary_payload_bytes = 12;
        return core::Status::OK;
    };
    auto append_binary_line = [&](std::string_view line) -> core::Status {
        BinaryCopyRow row;
        if (!parseCanonicalCopyLine(line, row)) {
            return send_copy_fail("COPY binary rowset conversion failed for canonical input row",
                                  core::Status::INVALID_ARGUMENT);
        }
        if (row.empty()) {
            return core::Status::OK;
        }
        size_t row_bytes = 4;
        for (const auto& field : row) {
            row_bytes += 4 + field.name.size() + 1 + 4 + field.value.size();
        }
        binary_payload_bytes += row_bytes;
        binary_rows.push_back(std::move(row));
        return flush_binary_rows(false);
    };
    auto flush_ready_rows = [&]() -> core::Status {
        while (pending.size() >= chunk_size) {
            size_t split = pending.rfind('\n', chunk_size);
            if (split == std::string::npos) {
                split = pending.find('\n', chunk_size);
            }
            if (split == std::string::npos) {
                break;
            }
            ++split;
            core::Status status = core::Status::OK;
            if (binary_copy) {
                size_t start = 0;
                while (start < split) {
                    const size_t end = pending.find('\n', start);
                    const size_t line_end = end == std::string::npos || end > split ? split : end;
                    status = append_binary_line(std::string_view(pending.data() + start,
                                                                 line_end - start));
                    if (status != core::Status::OK) {
                        return status;
                    }
                    if (end == std::string::npos || end >= split) {
                        break;
                    }
                    start = end + 1;
                }
            } else {
                status = send_payload(reinterpret_cast<const uint8_t*>(pending.data()), split);
                if (status != core::Status::OK) {
                    return status;
                }
            }
            pending.erase(0, split);
        }
        return core::Status::OK;
    };

    while (*copy_input_stream_) {
        const int64_t read_started = phase_trace ? driverPhaseNowNs() : 0;
        copy_input_stream_->read(reinterpret_cast<char*>(buffer.data()),
                                 static_cast<std::streamsize>(buffer.size()));
        if (phase_trace) {
            read_elapsed_ns += driverPhaseNowNs() - read_started;
        }
        const auto bytes_read = copy_input_stream_->gcount();
        if (bytes_read > 0) {
            total_read += static_cast<size_t>(bytes_read);
            pending.append(reinterpret_cast<const char*>(buffer.data()),
                           static_cast<size_t>(bytes_read));
            auto status = flush_ready_rows();
            if (status != core::Status::OK) {
                return status;
            }
        }
        if (copy_input_stream_->eof()) {
            break;
        }
    }
    if (copy_input_stream_->bad()) {
        const std::string message = "COPY input stream read failed";
        return send_copy_fail(message, core::Status::IO_ERROR);
    }
    if (!pending.empty()) {
        if (binary_copy) {
            size_t start = 0;
            while (start <= pending.size()) {
                const size_t end = pending.find('\n', start);
                const size_t line_end = end == std::string::npos ? pending.size() : end;
                auto status = append_binary_line(
                    std::string_view(pending.data() + start, line_end - start));
                if (status != core::Status::OK) {
                    return status;
                }
                if (end == std::string::npos) {
                    break;
                }
                start = end + 1;
            }
        } else {
            auto status = send_payload(reinterpret_cast<const uint8_t*>(pending.data()),
                                       pending.size());
            if (status != core::Status::OK) {
                return status;
            }
        }
    }
    if (binary_copy) {
        auto status = flush_binary_rows(true);
        if (status != core::Status::OK) {
            return status;
        }
    }
    const auto done_payload = protocol::buildCopyDonePayload();
    const int64_t done_started = phase_trace ? driverPhaseNowNs() : 0;
    auto done_status =
        sendMessage(protocol::MessageType::CopyDone, done_payload, 0, false, nullptr, ctx);
    if (phase_trace) {
        const int64_t done_elapsed_ns = driverPhaseNowNs() - done_started;
        writeDriverPhaseTrace("copy_input_stream",
                              "read_input_total",
                              read_elapsed_ns,
                              total_read,
                              1,
                              protocol::MessageType::CopyData,
                              0,
                              tls_active_);
        writeDriverPhaseTrace("copy_input_stream",
                              "send_copy_data_total",
                              send_data_elapsed_ns,
                              total_sent,
                              chunk_count,
                              protocol::MessageType::CopyData,
                              0,
                              tls_active_);
        writeDriverPhaseTrace("copy_input_stream",
                              "send_copy_done",
                              done_elapsed_ns,
                              done_payload.size(),
                              1,
                              protocol::MessageType::CopyDone,
                              0,
                              tls_active_);
        writeDriverPhaseTrace("copy_input_stream",
                              "total",
                              driverPhaseNowNs() - total_started,
                              total_sent,
                              chunk_count,
                              protocol::MessageType::CopyData,
                              0,
                              tls_active_);
    }
    return done_status;
}

core::Status NetworkClient::handleCopyOutResponseMessage(const protocol::ProtocolMessage& msg,
                                                         NetworkResultSet& results,
                                                         core::ErrorContext* ctx) {
    protocol::CopyOutResponse response;
    auto status = protocol::parseCopyOutResponse(msg.body, response, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    results.copy_active = true;
    results.copy_format = response.format;
    results.copy_column_formats = std::move(response.column_formats);
    return core::Status::OK;
}

core::Status NetworkClient::handleCopyBothResponseMessage(const protocol::ProtocolMessage& msg,
                                                          NetworkResultSet& results,
                                                          core::ErrorContext* ctx) {
    protocol::CopyBothResponse response;
    auto status = protocol::parseCopyBothResponse(msg.body, response, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    results.copy_active = true;
    results.copy_format = response.format;
    return core::Status::OK;
}

core::Status NetworkClient::handleCopyDataMessage(const protocol::ProtocolMessage& msg,
                                                  NetworkResultSet& results,
                                                  core::ErrorContext* ctx) {
    protocol::CopyData data;
    auto status = protocol::parseCopyData(msg.body, data, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    if (!data.data.empty()) {
        if (copy_output_stream_) {
            copy_output_stream_->write(reinterpret_cast<const char*>(data.data.data()),
                                       static_cast<std::streamsize>(data.data.size()));
            if (!*copy_output_stream_) {
                return setError(ctx, core::Status::IO_ERROR, "COPY output stream write failed");
            }
        }
        results.copy_data.insert(results.copy_data.end(), data.data.begin(), data.data.end());
    }
    return core::Status::OK;
}

core::Status NetworkClient::handleCopyFailMessage(const protocol::ProtocolMessage& msg,
                                                  core::ErrorContext* ctx) {
    protocol::CopyFailInfo fail;
    auto status = protocol::parseCopyFail(msg.body, fail, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    return setError(ctx,
                    core::Status::INTERNAL_ERROR,
                    fail.error_message.empty() ? "COPY failed" : "COPY failed: " + fail.error_message);
}

core::Status NetworkClient::handleAsyncMessage(const protocol::ProtocolMessage& msg,
                                               core::ErrorContext* ctx) {
    switch (msg.header.type) {
        case protocol::MessageType::ParameterStatus: {
            std::vector<std::pair<std::string, std::string>> values;
            auto status = protocol::parseParameterStatuses(msg.body, values, ctx);
            if (status == core::Status::OK) {
                for (const auto& [name, value] : values) {
                    parameter_status_[name] = value;
                    if (name == "attachment_id") {
                        auto uuid = parseUuidHex(value);
                        if (uuid.size() == session_id_.size()) {
                            std::copy(uuid.begin(), uuid.end(), session_id_.begin());
                        }
                    } else if (name == "current_txn_id") {
                        current_txn_id_ = static_cast<uint64_t>(std::strtoull(value.c_str(), nullptr, 10));
                    }
                }
            }
            return status;
        }
        case protocol::MessageType::Notification: {
            protocol::Notification note;
            auto status = protocol::parseNotification(msg.body, note, ctx);
            if (status == core::Status::OK) {
                Notification client_note;
                client_note.process_id = note.process_id;
                client_note.channel = note.channel;
                client_note.payload = std::move(note.payload);
                client_note.change_type = note.change_type;
                client_note.row_id = note.row_id;
                notifications_.push_back(std::move(client_note));
            }
            return status;
        }
        case protocol::MessageType::QueryPlan: {
            auto plan = std::make_unique<protocol::QueryPlan>();
            auto status = protocol::parseQueryPlan(msg.body, *plan, ctx);
            if (status == core::Status::OK) {
                last_plan_ = std::move(plan);
            }
            return status;
        }
        case protocol::MessageType::SblrCompiled: {
            auto compiled = std::make_unique<protocol::SblrCompiled>();
            auto status = protocol::parseSblrCompiled(msg.body, *compiled, ctx);
            if (status == core::Status::OK) {
                last_sblr_ = std::move(compiled);
            }
            return status;
        }
        default:
            return core::Status::OK;
    }
}

core::Status NetworkClient::handleTxnStatusMessage(const protocol::ProtocolMessage& msg,
                                                   core::ErrorContext* ctx) {
    protocol::TxnFinalityStatus status;
    auto parsed = protocol::parseTxnFinalityStatus(msg.body, status, ctx);
    if (parsed != core::Status::OK) {
        return parsed;
    }
    last_commit_finality_ = toNetworkFinality(status);
    last_commit_finality_present_ = true;
    return core::Status::OK;
}

core::Status NetworkClient::handleErrorResponse(const protocol::ProtocolMessage& msg,
                                                core::ErrorContext* ctx) {
    const core::Status mapped = mapProtocolError(msg, ctx);
    while (true) {
        protocol::ProtocolMessage trailing;
        core::ErrorContext trailing_ctx;
        auto status = receiveMessage(trailing, &trailing_ctx);
        if (status != core::Status::OK) {
            last_query_sequence_ = 0;
            return mapped;
        }
        traceWireEvent("error_drain_recv",
                       trailing.header.type,
                       trailing.header.sequence,
                       in_transaction_);
        if (handleAsyncMessage(trailing, &trailing_ctx) == core::Status::OK &&
            isAsyncMessageType(trailing.header.type)) {
            continue;
        }
        if (trailing.header.type == protocol::MessageType::Error) {
            continue;
        }
        if (trailing.header.type == protocol::MessageType::TxnStatus) {
            (void)handleTxnStatusMessage(trailing, &trailing_ctx);
            continue;
        }
        if (trailing.header.type == protocol::MessageType::Ready) {
            core::ErrorContext ready_ctx;
            const auto ready_status =
                parseReadyAndTrackTransaction(trailing.body, in_transaction_, current_txn_id_, &ready_ctx);
            last_query_sequence_ = 0;
            return ready_status == core::Status::OK ? mapped : ready_status;
        }
    }
}

core::Status NetworkClient::drainUntilReady(std::string* command_tag,
                                            uint64_t* rows,
                                            uint64_t* last_id,
                                            core::ErrorContext* ctx) {
    std::string tag;
    uint64_t local_rows = 0;
    uint64_t local_last = 0;
    while (true) {
        protocol::ProtocolMessage msg;
        auto status = receiveMessage(msg, ctx);
        if (status != core::Status::OK) {
            return status;
        }
        traceWireEvent("drain_until_ready_recv",
                       msg.header.type,
                       msg.header.sequence,
                       in_transaction_);
        if (handleAsyncMessage(msg, ctx) == core::Status::OK &&
            isAsyncMessageType(msg.header.type)) {
            continue;
        }
        switch (msg.header.type) {
            case protocol::MessageType::CommandComplete: {
                uint8_t command_type = 0;
                status = protocol::parseCommandComplete(msg.body, command_type, local_rows, local_last, tag, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                break;
            }
            case protocol::MessageType::TxnStatus: {
                status = handleTxnStatusMessage(msg, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                break;
            }
            case protocol::MessageType::Error:
                return handleErrorResponse(msg, ctx);
            case protocol::MessageType::Ready: {
                auto ready_status = parseReadyAndTrackTransaction(msg.body, in_transaction_, current_txn_id_, ctx);
                if (ready_status != core::Status::OK) {
                    return ready_status;
                }
                if (command_tag) {
                    *command_tag = tag;
                }
                if (rows) {
                    *rows = local_rows;
                }
                if (last_id) {
                    *last_id = local_last;
                }
                last_query_sequence_ = 0;
                return core::Status::OK;
            }
            default:
                break;
        }
    }
}

core::Status NetworkClient::finishAutocommitStatement(bool statement_succeeded,
                                                      core::ErrorContext* ctx) {
    if (!config_.autocommit || explicit_transaction_active_ || !in_transaction_ ||
        serverAutocommitRequested()) {
        return core::Status::OK;
    }
    const protocol::MessageType finality_type =
        statement_succeeded ? protocol::MessageType::TxnCommit : protocol::MessageType::TxnRollback;
    traceWireEvent(statement_succeeded ? "autocommit_commit_send" : "autocommit_rollback_send",
                   finality_type,
                   next_sequence_,
                   in_transaction_);
    std::vector<uint8_t> payload;
    if (statement_succeeded) {
        protocol::TxnCommitRequest request;
        request.contract_flags = protocol::kTxnCommitFlagHasIdempotencyKey |
                                 protocol::kTxnCommitFlagStatementHasSideEffects;
        request.idempotency_key = randomProtocolUuid();
        request.request_fingerprint = fnv1a64("transaction.autocommit.commit");
        request.expected_txn_id = current_txn_id_;
        last_commit_finality_ = NetworkTransactionFinality{};
        last_commit_finality_.state = protocol::TxnFinalityState::Unknown;
        last_commit_finality_.idempotency_key = request.idempotency_key;
        last_commit_finality_.request_fingerprint = request.request_fingerprint;
        last_commit_finality_.original_txn_id = current_txn_id_;
        last_commit_finality_.diagnostic_code = "SBWP.AUTOCOMMIT.FINALITY_PENDING";
        last_commit_finality_.detail = "autocommit_outcome_pending_engine_finality_report";
        last_commit_finality_present_ = true;
        payload = protocol::buildTxnCommitPayload(request);
    } else {
        payload = protocol::buildTxnRollbackPayload(0);
    }
    auto status = sendMessage(finality_type, payload, 0, false, nullptr, ctx);
    if (status != core::Status::OK) {
        if (statement_succeeded) {
            last_commit_finality_.diagnostic_code =
                "SBWP.AUTOCOMMIT.SEND_FAILED_FINALITY_UNKNOWN";
            last_commit_finality_.detail =
                "autocommit_send_failed_before_engine_finality_report";
        }
        return status;
    }
    status = drainUntilReady(nullptr, nullptr, nullptr, ctx);
    if (statement_succeeded && status != core::Status::OK) {
        last_commit_finality_.state = protocol::TxnFinalityState::Unknown;
        last_commit_finality_.flags &=
            static_cast<uint16_t>(~protocol::kTxnFinalityFlagEngineKnown);
        last_commit_finality_.diagnostic_code =
            "SBWP.AUTOCOMMIT.READY_LOST_FINALITY_UNKNOWN";
        last_commit_finality_.detail =
            "autocommit_outcome_unknown_until_engine_finality_query";
    }
    return status;
}

bool NetworkClient::serverAutocommitRequested() const {
    return server_autocommit_requested_;
}

core::Status NetworkClient::readyAfterStatement(const std::vector<uint8_t>& payload,
                                                core::ErrorContext* ctx) {
    const bool server_finality_requested = serverAutocommitRequested();
    const uint64_t transaction_before_ready = current_txn_id_;
    auto status = parseReadyAndTrackTransaction(payload, in_transaction_, current_txn_id_, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    const bool server_finality_completed =
        server_finality_requested && current_txn_id_ != transaction_before_ready;
    if (server_finality_requested && !server_finality_completed) {
        server_autocommit_requested_ = false;
    }
    status = finishAutocommitStatement(true, ctx);
    if (server_finality_requested) {
        server_autocommit_requested_ = false;
        last_query_sequence_ = 0;
    }
    return status;
}

core::Status NetworkClient::errorAfterStatement(const protocol::ProtocolMessage& msg,
                                                core::ErrorContext* ctx) {
    const bool server_finality_requested = serverAutocommitRequested();
    const uint64_t transaction_before_error = current_txn_id_;
    const core::Status statement_status = handleErrorResponse(msg, ctx);
    const bool server_finality_completed =
        server_finality_requested && current_txn_id_ != transaction_before_error;
    if (server_finality_requested && !server_finality_completed) {
        server_autocommit_requested_ = false;
    }
    const auto finality_status = finishAutocommitStatement(false, ctx);
    if (server_finality_requested) {
        server_autocommit_requested_ = false;
        last_query_sequence_ = 0;
    }
    return finality_status == core::Status::OK ? statement_status : finality_status;
}

core::Status NetworkClient::sendMessage(protocol::MessageType type,
                                        const std::vector<uint8_t>& payload,
                                        uint8_t flags,
                                        bool force_zero,
                                        uint32_t* sequence_out,
                                        core::ErrorContext* ctx) {
    protocol::ProtocolMessage msg;
    msg.header.type = type;
    msg.header.flags = flags;
    msg.header.sequence = force_zero ? 0 : next_sequence_++;
    if (sequence_out) {
        *sequence_out = msg.header.sequence;
    }
    if (!force_zero) {
        msg.header.attachment_id = session_id_;
        msg.header.txn_id = current_txn_id_;
    }
    msg.body = payload;
    return sendMessage(msg, ctx);
}

core::Status NetworkClient::handshake(core::ErrorContext* ctx) {
    next_sequence_ = 1;
    parameter_status_.clear();
    session_id_.fill(0);
    last_commit_finality_ = NetworkTransactionFinality{};
    last_commit_finality_present_ = false;

    uint64_t features = 0;
    std::map<std::string, std::string> params;
    auto status = buildStartupParams(features, params, ctx);
    if (status != core::Status::OK) {
        return status;
    }

    const uint64_t required_features =
        config_.enable_copy_streaming
            ? (protocol::kFeatureStreaming | protocol::kFeatureBinaryCopy)
            : 0;
    auto payload = protocol::buildP1StartupPayload(features, required_features, params);
    status = sendMessage(protocol::MessageType::Startup, payload, 0, true, nullptr, ctx);
    if (status != core::Status::OK) {
        return status;
    }

    std::unique_ptr<ScramClient> scram;
    while (true) {
        protocol::ProtocolMessage msg;
        status = receiveMessage(msg, ctx);
        if (status != core::Status::OK) {
            return status;
        }
        if (handleAsyncMessage(msg, ctx) == core::Status::OK &&
            (msg.header.type == protocol::MessageType::ParameterStatus ||
             msg.header.type == protocol::MessageType::Notification ||
             msg.header.type == protocol::MessageType::QueryPlan ||
             msg.header.type == protocol::MessageType::SblrCompiled)) {
            continue;
        }
        switch (msg.header.type) {
            case protocol::MessageType::NegotiateVersion:
                continue;
            case protocol::MessageType::AuthRequest: {
                protocol::AuthMethod method;
                std::vector<uint8_t> data;
                status = protocol::parseAuthRequest(msg.body, method, data, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                if (method == protocol::AuthMethod::Ok) {
                    continue;
                }
                resolved_auth_context_.resolved_auth_method = authMethodName(method);
                resolved_auth_context_.resolved_auth_plugin_id =
                    authPluginIdForMethod(method, config_.auth_method_id);
                if (method == protocol::AuthMethod::Password) {
                    std::vector<uint8_t> resp(config_.password.begin(), config_.password.end());
                    status = sendMessage(protocol::MessageType::AuthResponse, resp, 0, true, nullptr, ctx);
                    if (status != core::Status::OK) {
                        return status;
                    }
                    continue;
                }
                if (method == protocol::AuthMethod::ScramSha256 ||
                    method == protocol::AuthMethod::ScramSha512) {
                    if (!scram) {
                        scram = std::make_unique<ScramClient>();
                        scram->method = method;
                    }
                    std::string first = scram->clientFirst(config_.username);
                    std::vector<uint8_t> resp(first.begin(), first.end());
                    status = sendMessage(protocol::MessageType::AuthResponse, resp, 0, true, nullptr, ctx);
                    if (status != core::Status::OK) {
                        return status;
                    }
                    continue;
                }
                if (method == protocol::AuthMethod::Token) {
                    std::vector<uint8_t> resp;
                    status = resolveTokenAuthPayload(config_, resp, ctx);
                    if (status != core::Status::OK) {
                        return status;
                    }
                    status = sendMessage(protocol::MessageType::AuthResponse, resp, 0, true, nullptr, ctx);
                    if (status != core::Status::OK) {
                        return status;
                    }
                    continue;
                }
                if (method == protocol::AuthMethod::Md5 ||
                    method == protocol::AuthMethod::Peer ||
                    method == protocol::AuthMethod::Reattach ||
                    method == protocol::AuthMethod::Certificate ||
                    method == protocol::AuthMethod::Gssapi ||
                    method == protocol::AuthMethod::Sspi ||
                    method == protocol::AuthMethod::Ldap ||
                    method == protocol::AuthMethod::Saml ||
                    method == protocol::AuthMethod::Oidc ||
                    method == protocol::AuthMethod::MfaTotp ||
                    method == protocol::AuthMethod::ClusterPki) {
                    return setFeatureNotSupported(ctx, "negotiated auth method is not locally executable in the C/C++ lane");
                }
                return setFeatureNotSupported(ctx, "unsupported auth method");
            }
            case protocol::MessageType::AuthContinue: {
                protocol::AuthMethod method;
                uint8_t stage = 0;
                std::vector<uint8_t> data;
                status = protocol::parseAuthContinue(msg.body, method, stage, data, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                if (method == protocol::AuthMethod::ScramSha256 ||
                    method == protocol::AuthMethod::ScramSha512) {
                    if (!scram) {
                        return setError(ctx,
                                        core::Status::INVALID_AUTHORIZATION,
                                        "SCRAM state missing");
                    }
                    std::string client_final;
                    std::string error;
                    if (!scram->handleServerFirst(config_.password,
                                                  std::string(data.begin(), data.end()),
                                                  client_final,
                                                  error)) {
                        return setError(ctx, core::Status::INVALID_AUTHORIZATION, error);
                    }
                    std::vector<uint8_t> resp(client_final.begin(), client_final.end());
                    status = sendMessage(protocol::MessageType::AuthResponse, resp, 0, true, nullptr, ctx);
                    if (status != core::Status::OK) {
                        return status;
                    }
                    continue;
                }
                if (method == protocol::AuthMethod::Token) {
                    std::vector<uint8_t> resp;
                    status = resolveTokenAuthPayload(config_, resp, ctx);
                    if (status != core::Status::OK) {
                        return status;
                    }
                    status = sendMessage(protocol::MessageType::AuthResponse, resp, 0, true, nullptr, ctx);
                    if (status != core::Status::OK) {
                        return status;
                    }
                    continue;
                }
                return setFeatureNotSupported(ctx, "unsupported auth continue method");
            }
            case protocol::MessageType::AuthOk: {
                std::vector<uint8_t> session_id;
                std::vector<uint8_t> info;
                status = protocol::parseAuthOk(msg.body, session_id, info, ctx);
                if (status != core::Status::OK) {
                    return status;
                }
                if (msg.header.attachment_id.size() == session_id_.size() &&
                    !isZeroUuidBytes(msg.header.attachment_id)) {
                    session_id_ = msg.header.attachment_id;
                } else if (session_id.size() == session_id_.size()) {
                    std::copy(session_id.begin(), session_id.end(), session_id_.begin());
                }
                if (isZeroUuidBytes(session_id_)) {
                    session_id_ = randomProtocolUuid();
                }
                if (scram && !info.empty()) {
                    std::string info_str(info.begin(), info.end());
                    if (!scram->verifyServerFinal(info_str)) {
                        return setError(ctx, core::Status::INVALID_AUTHORIZATION, "SCRAM verification failed");
                    }
                }
                continue;
            }
            case protocol::MessageType::Ready:
                return parseReadyAndTrackTransaction(msg.body, in_transaction_, current_txn_id_, ctx);
            case protocol::MessageType::Error:
                return mapProtocolError(msg, ctx);
            default:
                break;
        }
    }
}

} // namespace client
} // namespace scratchbird
