// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "nlohmann/json.hpp"
#define private public
#include "scratchbird/client/connection.h"
#include "scratchbird/client/driver_config.h"
#include "scratchbird/client/metadata.h"
#include "scratchbird/client/network_client.h"
#undef private
#include "scratchbird/core/error_context.h"
#include "scratchbird/core/status.h"
#include "scratchbird/network/network.h"
#include "scratchbird/network/socket.h"
#include "scratchbird/client/pool.h"
#include "scratchbird/client/leak_detector.h"
#include "scratchbird/client/scratchbird_client.h"
#include "scratchbird/protocol/sbwp_protocol.h"

namespace {

class NetworkEnvironment final : public ::testing::Environment {
public:
    void SetUp() override {
        ASSERT_TRUE(scratchbird::network::initNetwork());
    }

    void TearDown() override {
        scratchbird::network::cleanupNetwork();
    }
};

[[maybe_unused]] ::testing::Environment* const kNetworkEnvironment =
    ::testing::AddGlobalTestEnvironment(new NetworkEnvironment());

uint64_t readU64Le(const uint8_t* data) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[i]) << (8 * i);
    }
    return value;
}

uint16_t readU16Le(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readU32Le(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

void writeU16Le(std::vector<uint8_t>& payload, size_t offset, uint16_t value) {
    payload[offset] = static_cast<uint8_t>(value & 0xFF);
    payload[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void writeU32Le(std::vector<uint8_t>& payload, size_t offset, uint32_t value) {
    payload[offset] = static_cast<uint8_t>(value & 0xFF);
    payload[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    payload[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    payload[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void writeU64Le(std::vector<uint8_t>& payload, size_t offset, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        payload[offset + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
    }
}

std::vector<uint8_t> encodeI32Le(int32_t value) {
    std::vector<uint8_t> bytes(4, 0);
    writeU32Le(bytes, 0, static_cast<uint32_t>(value));
    return bytes;
}

std::vector<uint8_t> encodeI64Le(int64_t value) {
    std::vector<uint8_t> bytes(8, 0);
    writeU64Le(bytes, 0, static_cast<uint64_t>(value));
    return bytes;
}

bool parseStartupPayload(const std::vector<uint8_t>& payload,
                         uint64_t& features_out,
                         std::unordered_map<std::string, std::string>& params_out,
                         std::string& error_out) {
    if (payload.size() < 12) {
        error_out = "startup payload truncated";
        return false;
    }

    if (payload.size() >= 84) {
        const uint32_t param_count = readU32Le(payload.data() + 80);
        if (param_count < 1024) {
            features_out = readU64Le(payload.data() + 8);
            params_out.clear();
            size_t offset = 84;
            for (uint32_t i = 0; i < param_count; ++i) {
                if (offset + 4 > payload.size()) {
                    error_out = "startup payload missing P1 key length";
                    return false;
                }
                const uint32_t key_len = readU32Le(payload.data() + offset);
                offset += 4;
                if (offset + key_len + 6 > payload.size()) {
                    error_out = "startup payload truncated P1 parameter";
                    return false;
                }
                std::string key(reinterpret_cast<const char*>(payload.data() + offset), key_len);
                offset += key_len;
                offset += 2;  // value kind and flags
                const uint32_t value_len = readU32Le(payload.data() + offset);
                offset += 4;
                if (offset + value_len > payload.size()) {
                    error_out = "startup payload truncated P1 value";
                    return false;
                }
                std::string value(reinterpret_cast<const char*>(payload.data() + offset), value_len);
                offset += value_len;
                params_out[key] = value;
            }
            if (offset + 4 <= payload.size() && readU32Le(payload.data() + offset) == 0) {
                return true;
            }
            error_out = "startup payload missing P1 terminator";
            return false;
        }
    }

    features_out = readU64Le(payload.data() + 4);
    params_out.clear();

    size_t offset = 12;
    while (offset < payload.size()) {
        size_t key_end = offset;
        while (key_end < payload.size() && payload[key_end] != 0) {
            ++key_end;
        }
        if (key_end >= payload.size()) {
            error_out = "startup payload missing key terminator";
            return false;
        }
        if (key_end == offset) {
            return true;
        }

        std::string key(reinterpret_cast<const char*>(payload.data() + offset), key_end - offset);
        offset = key_end + 1;

        size_t value_end = offset;
        while (value_end < payload.size() && payload[value_end] != 0) {
            ++value_end;
        }
        if (value_end >= payload.size()) {
            error_out = "startup payload missing value terminator";
            return false;
        }

        std::string value(reinterpret_cast<const char*>(payload.data() + offset), value_end - offset);
        params_out[key] = value;
        offset = value_end + 1;
    }

    error_out = "startup payload missing final terminator";
    return false;
}

scratchbird::core::Status readMessage(scratchbird::network::Socket* socket,
                                      scratchbird::protocol::ProtocolMessage& msg,
                                      scratchbird::core::ErrorContext* ctx) {
    if (!socket) {
        if (ctx) {
            ctx->message = "socket not set";
        }
        return scratchbird::core::Status::INVALID_ARGUMENT;
    }
    std::array<uint8_t, scratchbird::protocol::kHeaderSize> header_buf{};
    auto status = socket->readExact(header_buf.data(), header_buf.size(), ctx);
    if (status != scratchbird::core::Status::OK) {
        return status;
    }

    std::vector<uint8_t> header_bytes(header_buf.begin(), header_buf.end());
    status = scratchbird::protocol::decodeHeader(header_bytes, msg.header, ctx);
    if (status != scratchbird::core::Status::OK) {
        return status;
    }

    msg.body.clear();
    if (msg.header.length > 0) {
        msg.body.resize(msg.header.length);
        status = socket->readExact(msg.body.data(), msg.body.size(), ctx);
        if (status != scratchbird::core::Status::OK) {
            return status;
        }
    }

    return scratchbird::core::Status::OK;
}

scratchbird::core::Status sendMessage(scratchbird::network::Socket* socket,
                                      scratchbird::protocol::MessageType type,
                                      const std::vector<uint8_t>& payload,
                                      uint32_t sequence,
                                      scratchbird::core::ErrorContext* ctx) {
    if (!socket) {
        if (ctx) {
            ctx->message = "socket not set";
        }
        return scratchbird::core::Status::INVALID_ARGUMENT;
    }

    scratchbird::protocol::MessageHeader header;
    header.type = type;
    header.flags = 0;
    header.length = static_cast<uint32_t>(payload.size());
    header.sequence = sequence;
    auto encoded = scratchbird::protocol::encodeMessage(header, payload);
    return socket->writeExact(encoded.data(), encoded.size(), ctx);
}

std::vector<uint8_t> buildAuthRequestPayload(scratchbird::protocol::AuthMethod method) {
    std::vector<uint8_t> payload(4, 0);
    payload[0] = static_cast<uint8_t>(method);
    return payload;
}

std::vector<uint8_t> buildAuthContinuePayload(scratchbird::protocol::AuthMethod method,
                                              uint8_t stage,
                                              const std::vector<uint8_t>& data) {
    std::vector<uint8_t> payload(8 + data.size(), 0);
    payload[0] = static_cast<uint8_t>(method);
    payload[1] = stage;
    writeU32Le(payload, 4, static_cast<uint32_t>(data.size()));
    if (!data.empty()) {
        std::memcpy(payload.data() + 8, data.data(), data.size());
    }
    return payload;
}

std::vector<uint8_t> buildAuthOkPayload(const std::vector<uint8_t>& info = {}) {
    std::vector<uint8_t> payload(20 + info.size(), 0);
    payload[0] = 0x42;
    writeU32Le(payload, 16, static_cast<uint32_t>(info.size()));
    if (!info.empty()) {
        std::memcpy(payload.data() + 20, info.data(), info.size());
    }
    return payload;
}

std::string base64EncodeString(const std::vector<uint8_t>& data) {
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

std::vector<uint8_t> base64DecodeString(const std::string& input) {
    if (input.empty()) {
        return {};
    }
    size_t out_len = (input.size() / 4) * 3;
    std::vector<uint8_t> out(out_len, 0);
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

bool scramHmacForTest(const std::vector<uint8_t>& key,
                      const std::string& data,
                      const EVP_MD* md,
                      std::vector<uint8_t>& out) {
    unsigned int len = EVP_MD_size(md);
    out.assign(len, 0);
    if (!HMAC(md,
              key.data(),
              static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char*>(data.data()),
              static_cast<int>(data.size()),
              out.data(),
              &len)) {
        return false;
    }
    out.resize(len);
    return true;
}

bool scramHashForTest(const std::vector<uint8_t>& data,
                      const EVP_MD* md,
                      std::vector<uint8_t>& out) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return false;
    }
    unsigned int len = EVP_MD_size(md);
    out.assign(len, 0);
    bool ok = EVP_DigestInit_ex(ctx, md, nullptr) == 1 &&
              EVP_DigestUpdate(ctx, data.data(), data.size()) == 1 &&
              EVP_DigestFinal_ex(ctx, out.data(), &len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok) {
        out.clear();
        return false;
    }
    out.resize(len);
    return true;
}

bool scramSaltedPasswordForTest(const std::string& password,
                                const std::vector<uint8_t>& salt,
                                uint32_t iterations,
                                const EVP_MD* md,
                                std::vector<uint8_t>& out) {
    const int hash_len = EVP_MD_size(md);
    out.assign(static_cast<size_t>(hash_len), 0);
    return PKCS5_PBKDF2_HMAC(password.c_str(),
                             static_cast<int>(password.size()),
                             salt.data(),
                             static_cast<int>(salt.size()),
                             static_cast<int>(iterations),
                             md,
                             hash_len,
                             out.data()) == 1;
}

std::string computeScramServerFinal(const std::string& password,
                                    const std::string& client_first_message,
                                    const std::string& server_first_message,
                                    const EVP_MD* md) {
    if (client_first_message.rfind("n,,", 0) != 0) {
        return "";
    }
    const std::string client_first_bare = client_first_message.substr(3);
    const size_t r_pos = server_first_message.find("r=");
    const size_t s_pos = server_first_message.find(",s=");
    const size_t i_pos = server_first_message.find(",i=");
    if (r_pos != 0 || s_pos == std::string::npos || i_pos == std::string::npos) {
        return "";
    }
    const std::string nonce = server_first_message.substr(2, s_pos - 2);
    const std::string salt_b64 =
        server_first_message.substr(s_pos + 3, i_pos - (s_pos + 3));
    const std::string iter_str = server_first_message.substr(i_pos + 3);
    uint32_t iterations = 0;
    try {
        iterations = static_cast<uint32_t>(std::stoul(iter_str));
    } catch (...) {
        return "";
    }
    auto salt = base64DecodeString(salt_b64);
    if (salt.empty()) {
        return "";
    }
    std::vector<uint8_t> salted_password;
    if (!scramSaltedPasswordForTest(password, salt, iterations, md, salted_password)) {
        return "";
    }
    const std::string client_final_without_proof = "c=biws,r=" + nonce;
    const std::string auth_message =
        client_first_bare + "," + server_first_message + "," + client_final_without_proof;
    std::vector<uint8_t> server_key;
    if (!scramHmacForTest(salted_password, "Server Key", md, server_key)) {
        return "";
    }
    std::vector<uint8_t> server_signature;
    if (!scramHmacForTest(server_key, auth_message, md, server_signature)) {
        return "";
    }
    return "v=" + base64EncodeString(server_signature);
}

std::vector<uint8_t> buildReadyPayload(uint8_t status = 1,
                                       uint64_t txn_id = 1,
                                       uint64_t epoch = 1) {
    std::vector<uint8_t> payload(20, 0);
    payload[0] = status;
    writeU64Le(payload, 4, txn_id);
    writeU64Le(payload, 12, epoch);
    return payload;
}

std::vector<uint8_t> buildCommandCompletePayload(uint8_t command_type,
                                                 uint64_t rows,
                                                 uint64_t last_id,
                                                 const std::string& tag) {
    std::vector<uint8_t> payload(20 + tag.size() + 1, 0);
    payload[0] = command_type;
    writeU64Le(payload, 4, rows);
    writeU64Le(payload, 12, last_id);
    std::memcpy(payload.data() + 20, tag.data(), tag.size());
    return payload;
}

std::vector<uint8_t> buildParameterDescriptionPayload(const std::vector<uint32_t>& type_oids) {
    std::vector<uint8_t> payload(4 + type_oids.size() * 4, 0);
    writeU16Le(payload, 0, static_cast<uint16_t>(type_oids.size()));
    size_t offset = 4;
    for (uint32_t oid : type_oids) {
        payload[offset + 0] = static_cast<uint8_t>(oid & 0xFF);
        payload[offset + 1] = static_cast<uint8_t>((oid >> 8) & 0xFF);
        payload[offset + 2] = static_cast<uint8_t>((oid >> 16) & 0xFF);
        payload[offset + 3] = static_cast<uint8_t>((oid >> 24) & 0xFF);
        offset += 4;
    }
    return payload;
}

struct RowColumnSpec {
    std::string name;
    uint32_t type_oid;
    int32_t type_modifier{0};
    uint8_t format{scratchbird::protocol::kFormatBinary};
    bool nullable{true};
};

std::vector<uint8_t> buildRowDescriptionPayload(const std::vector<RowColumnSpec>& columns) {
    size_t payload_len = 4;
    for (const auto& col : columns) {
        payload_len += 4 + col.name.size() + 4 + 2 + 4 + 2 + 4 + 1 + 1 + 2;
    }
    std::vector<uint8_t> payload(payload_len, 0);
    writeU16Le(payload, 0, static_cast<uint16_t>(columns.size()));
    size_t offset = 4;
    for (const auto& col : columns) {
        writeU32Le(payload, offset, static_cast<uint32_t>(col.name.size()));
        offset += 4;
        if (!col.name.empty()) {
            std::memcpy(payload.data() + offset, col.name.data(), col.name.size());
            offset += col.name.size();
        }
        writeU32Le(payload, offset, 0);
        offset += 4;
        writeU16Le(payload, offset, 0);
        offset += 2;
        writeU32Le(payload, offset, col.type_oid);
        offset += 4;
        writeU16Le(payload, offset, 0);
        offset += 2;
        writeU32Le(payload, offset, static_cast<uint32_t>(col.type_modifier));
        offset += 4;
        payload[offset++] = col.format;
        payload[offset++] = col.nullable ? 1 : 0;
        writeU16Le(payload, offset, 0);
        offset += 2;
    }
    return payload;
}

std::vector<uint8_t> buildDataRowPayload(
    const std::vector<std::optional<std::vector<uint8_t>>>& values) {
    const uint16_t count = static_cast<uint16_t>(values.size());
    const uint16_t null_bytes = static_cast<uint16_t>((values.size() + 7) / 8);
    size_t payload_len = 4 + null_bytes;
    for (const auto& value : values) {
        if (!value.has_value()) {
            continue;
        }
        payload_len += 4 + value->size();
    }

    std::vector<uint8_t> payload(payload_len, 0);
    writeU16Le(payload, 0, count);
    writeU16Le(payload, 2, null_bytes);
    size_t offset = 4;
    for (size_t i = 0; i < values.size(); ++i) {
        if (!values[i].has_value()) {
            payload[offset + (i / 8)] |= static_cast<uint8_t>(1u << (i % 8));
        }
    }
    offset += null_bytes;

    for (const auto& value : values) {
        if (!value.has_value()) {
            continue;
        }
        writeU32Le(payload, offset, static_cast<uint32_t>(value->size()));
        offset += 4;
        if (!value->empty()) {
            std::memcpy(payload.data() + offset, value->data(), value->size());
            offset += value->size();
        }
    }
    return payload;
}

std::vector<uint8_t> buildNotificationPayload(uint32_t process_id,
                                              const std::string& channel,
                                              const std::vector<uint8_t>& payload_bytes,
                                              uint8_t change_type = 0,
                                              uint64_t row_id = 0,
                                              bool include_row = false) {
    std::vector<uint8_t> payload(4 + 4 + channel.size() + 4 + payload_bytes.size() +
                                     (change_type != 0 || include_row ? 1 : 0) +
                                     (include_row ? 8 : 0),
                                 0);
    size_t offset = 0;
    writeU32Le(payload, offset, process_id);
    offset += 4;
    writeU32Le(payload, offset, static_cast<uint32_t>(channel.size()));
    offset += 4;
    if (!channel.empty()) {
        std::memcpy(payload.data() + offset, channel.data(), channel.size());
        offset += channel.size();
    }
    writeU32Le(payload, offset, static_cast<uint32_t>(payload_bytes.size()));
    offset += 4;
    if (!payload_bytes.empty()) {
        std::memcpy(payload.data() + offset, payload_bytes.data(), payload_bytes.size());
        offset += payload_bytes.size();
    }
    if (change_type != 0 || include_row) {
        payload[offset++] = change_type;
    }
    if (include_row) {
        writeU64Le(payload, offset, row_id);
    }
    return payload;
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

std::optional<std::string> integrationDsn() {
    const char* value = std::getenv("SCRATCHBIRD_TEST_DSN");
    if (!value || value[0] == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

struct NotificationProbe {
    int calls{0};
    std::string channel;
    std::string payload_hex;
    uint32_t process_id{0};
    uint8_t change_type{0};
    uint64_t row_id{0};
    int has_row_id{0};
};

void notificationProbeCallback(const char* channel,
                               const uint8_t* payload,
                               size_t payload_len,
                               uint32_t process_id,
                               uint8_t change_type,
                               uint64_t row_id,
                               int has_row_id,
                               void* user_data) {
    auto* probe = static_cast<NotificationProbe*>(user_data);
    if (!probe) {
        return;
    }
    probe->calls += 1;
    probe->channel = channel ? channel : "";
    probe->payload_hex = payload ? hexEncode(payload, payload_len) : "";
    probe->process_id = process_id;
    probe->change_type = change_type;
    probe->row_id = row_id;
    probe->has_row_id = has_row_id;
}

void appendErrorField(std::vector<uint8_t>& payload, uint8_t field, const std::string& value) {
    payload.push_back(field);
    payload.insert(payload.end(), value.begin(), value.end());
    payload.push_back(0);
}

std::vector<uint8_t> buildErrorPayload(const std::string& severity,
                                       const std::string& sqlstate,
                                       const std::string& message,
                                       const std::string& detail = std::string(),
                                       const std::string& hint = std::string()) {
    std::vector<uint8_t> payload;
    appendErrorField(payload, 'S', severity);
    appendErrorField(payload, 'C', sqlstate);
    appendErrorField(payload, 'M', message);
    if (!detail.empty()) {
        appendErrorField(payload, 'D', detail);
    }
    if (!hint.empty()) {
        appendErrorField(payload, 'H', hint);
    }
    payload.push_back(0);
    return payload;
}

bool parseQueryPayloadSql(const std::vector<uint8_t>& payload,
                          std::string& sql_out,
                          std::string& error_out) {
    if (payload.size() < 13) {
        error_out = "query payload truncated";
        return false;
    }
    size_t end = 12;
    while (end < payload.size() && payload[end] != 0) {
        ++end;
    }
    if (end >= payload.size()) {
        error_out = "query payload missing terminator";
        return false;
    }
    sql_out.assign(reinterpret_cast<const char*>(payload.data() + 12), end - 12);
    return true;
}

bool parseParsePayloadStatementAndSql(const std::vector<uint8_t>& payload,
                                      std::string& statement_out,
                                      std::string& sql_out,
                                      std::vector<uint32_t>& param_types_out,
                                      std::string& error_out) {
    if (payload.size() < 12) {
        error_out = "parse payload truncated";
        return false;
    }

    size_t offset = 0;
    const uint32_t stmt_len = readU32Le(payload.data() + offset);
    offset += 4;
    if (offset + stmt_len + 4 > payload.size()) {
        error_out = "parse payload statement truncated";
        return false;
    }
    statement_out.assign(reinterpret_cast<const char*>(payload.data() + offset), stmt_len);
    offset += stmt_len;

    const uint32_t sql_len = readU32Le(payload.data() + offset);
    offset += 4;
    if (offset + sql_len + 4 > payload.size()) {
        error_out = "parse payload sql truncated";
        return false;
    }
    sql_out.assign(reinterpret_cast<const char*>(payload.data() + offset), sql_len);
    offset += sql_len;

    if (offset + 4 > payload.size()) {
        error_out = "parse payload parameter section truncated";
        return false;
    }
    const uint16_t param_count = readU16Le(payload.data() + offset);
    offset += 4;
    param_types_out.clear();
    param_types_out.reserve(param_count);
    for (uint16_t index = 0; index < param_count; ++index) {
        if (offset + 4 > payload.size()) {
            error_out = "parse payload parameter type truncated";
            return false;
        }
        param_types_out.push_back(readU32Le(payload.data() + offset));
        offset += 4;
    }
    return true;
}

bool parseParsePayloadStatementAndSql(const std::vector<uint8_t>& payload,
                                      std::string& statement_out,
                                      std::string& sql_out,
                                      std::string& error_out) {
    std::vector<uint32_t> ignored_types;
    return parseParsePayloadStatementAndSql(
        payload, statement_out, sql_out, ignored_types, error_out);
}

bool parseTxnNamePayload(const std::vector<uint8_t>& payload,
                         std::string& name_out,
                         std::string& error_out) {
    if (payload.size() < 4) {
        error_out = "txn payload truncated";
        return false;
    }
    const uint32_t len = readU32Le(payload.data());
    if (payload.size() < 4 + len) {
        error_out = "txn payload name truncated";
        return false;
    }
    name_out.assign(reinterpret_cast<const char*>(payload.data() + 4), len);
    return true;
}

bool parseTxnBeginPayload(const std::vector<uint8_t>& payload,
                          uint16_t& flags_out,
                          uint8_t& conflict_action_out,
                          uint8_t& autocommit_mode_out,
                          uint8_t& isolation_level_out,
                          uint8_t& read_committed_mode_out,
                          uint8_t& access_mode_out,
                          uint8_t& deferrable_out,
                          uint8_t& wait_mode_out,
                          uint32_t& timeout_ms_out,
                          std::string& error_out) {
    if (payload.size() < 12) {
        error_out = "txn begin payload truncated";
        return false;
    }
    flags_out = readU16Le(payload.data());
    conflict_action_out = payload[2];
    autocommit_mode_out = payload[3];
    isolation_level_out = payload[4];
    read_committed_mode_out = 0;
    access_mode_out = payload[5];
    deferrable_out = payload[6];
    wait_mode_out = payload[7];
    timeout_ms_out = readU32Le(payload.data() + 8);
    if ((flags_out & SB_TXN_FLAG_HAS_READ_COMMITTED_MODE) != 0) {
        if (payload.size() < 13) {
            error_out = "txn begin payload missing read committed mode";
            return false;
        }
        read_committed_mode_out = payload[12];
    }
    return true;
}

std::array<uint8_t, 16> testUuid(uint8_t seed) {
    std::array<uint8_t, 16> uuid{};
    for (size_t i = 0; i < uuid.size(); ++i) {
        uuid[i] = static_cast<uint8_t>(seed + i);
    }
    uuid[6] = static_cast<uint8_t>((uuid[6] & 0x0Fu) | 0x70u);
    uuid[8] = static_cast<uint8_t>((uuid[8] & 0x3Fu) | 0x80u);
    return uuid;
}

bool isZeroUuid(const std::array<uint8_t, 16>& uuid) {
    return std::all_of(uuid.begin(), uuid.end(), [](uint8_t value) {
        return value == 0;
    });
}

scratchbird::protocol::TxnFinalityStatus makeTxnFinalityStatus(
    scratchbird::protocol::TxnFinalityState state,
    uint16_t flags,
    const std::array<uint8_t, 16>& idempotency_key,
    const std::array<uint8_t, 16>& finality_token,
    uint64_t request_fingerprint,
    uint64_t original_txn_id,
    uint64_t replacement_txn_id,
    std::string diagnostic_code,
    std::string detail) {
    scratchbird::protocol::TxnFinalityStatus status;
    status.state = state;
    status.flags = flags;
    status.idempotency_key = idempotency_key;
    status.finality_token = finality_token;
    status.request_fingerprint = request_fingerprint;
    status.original_txn_id = original_txn_id;
    status.replacement_txn_id = replacement_txn_id;
    status.diagnostic_code = std::move(diagnostic_code);
    status.detail = std::move(detail);
    return status;
}

std::vector<uint8_t> buildTxnFinalityStatusPayload(
    scratchbird::protocol::TxnFinalityState state,
    uint16_t flags,
    const std::array<uint8_t, 16>& idempotency_key,
    const std::array<uint8_t, 16>& finality_token,
    uint64_t request_fingerprint,
    uint64_t original_txn_id,
    uint64_t replacement_txn_id,
    const std::string& diagnostic_code,
    const std::string& detail) {
    return scratchbird::protocol::buildTxnFinalityStatusPayload(
        makeTxnFinalityStatus(state,
                              flags,
                              idempotency_key,
                              finality_token,
                              request_fingerprint,
                              original_txn_id,
                              replacement_txn_id,
                              diagnostic_code,
                              detail));
}

bool parseCancelPayload(const std::vector<uint8_t>& payload,
                        uint32_t& cancel_type_out,
                        uint32_t& sequence_out,
                        std::string& error_out) {
    if (payload.size() < 8) {
        error_out = "cancel payload truncated";
        return false;
    }
    cancel_type_out = readU32Le(payload.data());
    sequence_out = readU32Le(payload.data() + 4);
    return true;
}

bool parseSblrPayload(const std::vector<uint8_t>& payload,
                      uint64_t& hash_out,
                      uint16_t& param_count_out,
                      std::vector<uint32_t>& param_lengths_out,
                      std::string& error_out) {
    if (payload.size() < 16) {
        error_out = "sblr payload truncated";
        return false;
    }
    hash_out = readU64Le(payload.data());
    const uint32_t bytecode_len = readU32Le(payload.data() + 8);
    param_count_out = readU16Le(payload.data() + 12);
    size_t offset = 16 + bytecode_len;
    if (offset > payload.size()) {
        error_out = "sblr payload bytecode truncated";
        return false;
    }
    param_lengths_out.clear();
    for (uint16_t i = 0; i < param_count_out; ++i) {
        if (offset + 4 > payload.size()) {
            error_out = "sblr payload parameter length truncated";
            return false;
        }
        const uint32_t len = readU32Le(payload.data() + offset);
        offset += 4;
        param_lengths_out.push_back(len);
        if (len != 0xFFFFFFFFu) {
            if (offset + len > payload.size()) {
                error_out = "sblr payload parameter data truncated";
                return false;
            }
            offset += len;
        }
    }
    return true;
}

struct ScriptedResponse {
    scratchbird::protocol::MessageType type;
    std::vector<uint8_t> payload;
    std::function<std::vector<uint8_t>(const scratchbird::protocol::ProtocolMessage&)> payload_builder;
};

struct ScriptedExchange {
    scratchbird::protocol::MessageType request_type;
    std::vector<ScriptedResponse> responses;
    std::function<bool(const scratchbird::protocol::ProtocolMessage&, std::string&)> validate;
};

struct ServerHarnessConfig {
    scratchbird::protocol::AuthMethod auth_method{scratchbird::protocol::AuthMethod::Ok};
    std::string expected_auth_response;
    bool expect_auth_response{false};
    bool send_auth_continue{false};
    bool stop_after_auth_request{false};
    uint8_t auth_continue_stage{1};
    std::string auth_continue_payload;
    std::string expected_auth_continue_response;
    std::vector<ScriptedExchange> exchanges;
    bool drain_after_script{false};
};

struct ServerHarness {
    explicit ServerHarness(ServerHarnessConfig cfg = {}) : config(std::move(cfg)) {}
    ~ServerHarness() {
        stop();
    }

    ServerHarnessConfig config;
    std::unique_ptr<scratchbird::network::Socket> listener;
    uint16_t port = 0;
    std::thread thread;
    std::string error;
    uint64_t startup_features = 0;
    std::unordered_map<std::string, std::string> startup_params;
    bool saw_auth_response = false;
    size_t auth_response_count = 0;

    void start() {
        thread = std::thread([this]() { run(); });
    }

    void stop() {
        if (listener) {
            listener->close();
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

private:
    void run() {
        scratchbird::core::ErrorContext ctx;
        scratchbird::network::NetworkAddress client_addr;
        auto client = listener->accept(&client_addr, &ctx);
        if (!client) {
            error = "accept failed: " + ctx.message;
            return;
        }

        scratchbird::protocol::ProtocolMessage startup;
        auto status = readMessage(client.get(), startup, &ctx);
        if (status != scratchbird::core::Status::OK) {
            error = "read startup failed: " + ctx.message;
            return;
        }
        if (startup.header.type != scratchbird::protocol::MessageType::Startup) {
            error = "unexpected message type";
            return;
        }

        if (!parseStartupPayload(startup.body, startup_features, startup_params, error)) {
            return;
        }

        status = sendMessage(client.get(),
                             scratchbird::protocol::MessageType::AuthRequest,
                             buildAuthRequestPayload(config.auth_method),
                             1,
                             &ctx);
        if (status != scratchbird::core::Status::OK) {
            error = "write auth request failed: " + ctx.message;
            return;
        }
        if (config.stop_after_auth_request) {
            return;
        }

        const bool expect_first_auth_response =
            config.expect_auth_response ||
            config.send_auth_continue ||
            config.auth_method == scratchbird::protocol::AuthMethod::Password;

        if (expect_first_auth_response) {
            scratchbird::protocol::ProtocolMessage auth_response;
            status = readMessage(client.get(), auth_response, &ctx);
            if (status != scratchbird::core::Status::OK) {
                error = "read auth response failed: " + ctx.message;
                return;
            }
            if (auth_response.header.type != scratchbird::protocol::MessageType::AuthResponse) {
                error = "expected auth response";
                return;
            }
            saw_auth_response = true;
            auth_response_count++;
            if (!config.expected_auth_response.empty()) {
                std::vector<uint8_t> expected(config.expected_auth_response.begin(),
                                              config.expected_auth_response.end());
                if (auth_response.body != expected) {
                    error = "auth response payload mismatch";
                    return;
                }
            }
        }

        if (config.send_auth_continue) {
            std::vector<uint8_t> challenge(config.auth_continue_payload.begin(),
                                           config.auth_continue_payload.end());
            status = sendMessage(client.get(),
                                 scratchbird::protocol::MessageType::AuthContinue,
                                 buildAuthContinuePayload(config.auth_method,
                                                          config.auth_continue_stage,
                                                          challenge),
                                 2,
                                 &ctx);
            if (status != scratchbird::core::Status::OK) {
                error = "write auth continue failed: " + ctx.message;
                return;
            }

            scratchbird::protocol::ProtocolMessage auth_continue_response;
            status = readMessage(client.get(), auth_continue_response, &ctx);
            if (status != scratchbird::core::Status::OK) {
                error = "read auth continue response failed: " + ctx.message;
                return;
            }
            if (auth_continue_response.header.type != scratchbird::protocol::MessageType::AuthResponse) {
                error = "expected auth continue response";
                return;
            }
            saw_auth_response = true;
            auth_response_count++;
            if (!config.expected_auth_continue_response.empty()) {
                std::vector<uint8_t> expected_continue(
                    config.expected_auth_continue_response.begin(),
                    config.expected_auth_continue_response.end());
                if (auth_continue_response.body != expected_continue) {
                    error = "auth continue response payload mismatch";
                    return;
                }
            }
        }

        status = sendMessage(client.get(),
                             scratchbird::protocol::MessageType::AuthOk,
                             buildAuthOkPayload(),
                             3,
                             &ctx);
        if (status != scratchbird::core::Status::OK) {
            error = "write auth ok failed: " + ctx.message;
            return;
        }

        status = sendMessage(client.get(),
                             scratchbird::protocol::MessageType::Ready,
                             buildReadyPayload(),
                             4,
                             &ctx);
        if (status != scratchbird::core::Status::OK) {
            error = "write ready failed: " + ctx.message;
            return;
        }

        uint32_t next_sequence = 5;
        for (const auto& exchange : config.exchanges) {
            scratchbird::protocol::ProtocolMessage request;
            status = readMessage(client.get(), request, &ctx);
            if (status != scratchbird::core::Status::OK) {
                error = "read scripted request failed: " + ctx.message;
                return;
            }
            if (request.header.type != exchange.request_type) {
                error = "unexpected request type in scripted exchange";
                return;
            }
            if (exchange.validate && !exchange.validate(request, error)) {
                return;
            }
            for (const auto& response : exchange.responses) {
                const auto payload =
                    response.payload_builder ? response.payload_builder(request) : response.payload;
                status = sendMessage(client.get(), response.type, payload, next_sequence++, &ctx);
                if (status != scratchbird::core::Status::OK) {
                    error = "write scripted response failed: " + ctx.message;
                    return;
                }
            }
        }

        if (!config.drain_after_script) {
            return;
        }
        while (true) {
            scratchbird::protocol::ProtocolMessage ignored;
            status = readMessage(client.get(), ignored, &ctx);
            if (status != scratchbird::core::Status::OK) {
                return;
            }
        }
    }
};

constexpr uint32_t kManagerProtocolMagic = 0x42444253;
constexpr uint16_t kManagerProtocolVersion = 0x0101;
constexpr size_t kManagerHeaderSize = 12;
constexpr uint8_t kMsgConnectResponse = 0x02;
constexpr uint8_t kMsgAuthChallenge = 0x12;
constexpr uint8_t kMsgAuthResponse = 0x11;
constexpr uint8_t kMsgStatusResponse = 0x64;
constexpr uint8_t kMsgMcpHello = 0x65;
constexpr uint8_t kMsgMcpAuthStart = 0x66;
constexpr uint8_t kMsgMcpAuthContinue = 0x67;
constexpr uint8_t kMsgMcpDbConnect = 0x69;

std::vector<uint8_t> buildManagerAuthResponsePayload(bool ok, const std::string& error_message) {
    std::vector<uint8_t> payload(1 + 4 + 256, 0);
    payload[0] = ok ? 0 : 1;
    if (!ok) {
        const size_t copy_len = std::min<size_t>(255, error_message.size());
        std::memcpy(payload.data() + 5, error_message.data(), copy_len);
    }
    return payload;
}

std::vector<uint8_t> buildManagerConnectResponsePayload(bool ok, const std::string& error_message) {
    std::vector<uint8_t> payload(1 + 2 + 2 + 16 + 64 + 32, 0);
    payload[0] = ok ? 0 : 1;
    if (!ok) {
        std::vector<uint8_t> full(payload);
        std::vector<uint8_t> err_len(4, 0);
        writeU32Le(err_len, 0, static_cast<uint32_t>(error_message.size()));
        full.insert(full.end(), err_len.begin(), err_len.end());
        full.insert(full.end(), error_message.begin(), error_message.end());
        return full;
    }
    return payload;
}

bool sendManagerFrame(scratchbird::network::Socket* socket,
                      uint8_t type,
                      const std::vector<uint8_t>& payload,
                      std::string& error_out) {
    if (!socket) {
        error_out = "manager socket not set";
        return false;
    }
    std::vector<uint8_t> frame(kManagerHeaderSize + payload.size(), 0);
    writeU32Le(frame, 0, kManagerProtocolMagic);
    writeU16Le(frame, 4, kManagerProtocolVersion);
    frame[6] = type;
    frame[7] = 0;
    writeU32Le(frame, 8, static_cast<uint32_t>(payload.size()));
    if (!payload.empty()) {
        std::memcpy(frame.data() + kManagerHeaderSize, payload.data(), payload.size());
    }
    scratchbird::core::ErrorContext ctx;
    auto status = socket->writeExact(frame.data(), frame.size(), &ctx);
    if (status != scratchbird::core::Status::OK) {
        error_out = "write manager frame failed: " + ctx.message;
        return false;
    }
    return true;
}

bool readManagerFrame(scratchbird::network::Socket* socket,
                      uint8_t& type_out,
                      std::vector<uint8_t>& payload_out,
                      std::string& error_out) {
    if (!socket) {
        error_out = "manager socket not set";
        return false;
    }
    std::array<uint8_t, kManagerHeaderSize> header{};
    scratchbird::core::ErrorContext ctx;
    auto status = socket->readExact(header.data(), header.size(), &ctx);
    if (status != scratchbird::core::Status::OK) {
        error_out = "read manager header failed: " + ctx.message;
        return false;
    }
    if (readU32Le(header.data()) != kManagerProtocolMagic) {
        error_out = "manager frame magic mismatch";
        return false;
    }
    if (readU16Le(header.data() + 4) != kManagerProtocolVersion) {
        error_out = "manager frame version mismatch";
        return false;
    }
    type_out = header[6];
    const uint32_t payload_len = readU32Le(header.data() + 8);
    payload_out.assign(payload_len, 0);
    if (payload_len > 0) {
        status = socket->readExact(payload_out.data(), payload_out.size(), &ctx);
        if (status != scratchbird::core::Status::OK) {
            error_out = "read manager payload failed: " + ctx.message;
            return false;
        }
    }
    return true;
}

struct ManagerProxyHarnessConfig {
    bool fail_auth{false};
    bool fail_connect{false};
    bool require_challenge{false};
    bool stop_after_auth_probe{false};
    std::string expected_auth_token{"manager-token"};
};

struct ManagerProxyHarness {
    explicit ManagerProxyHarness(ManagerProxyHarnessConfig cfg = {})
        : config(std::move(cfg)) {}
    ~ManagerProxyHarness() {
        stop();
    }

    ManagerProxyHarnessConfig config;
    std::unique_ptr<scratchbird::network::Socket> listener;
    uint16_t port = 0;
    std::thread thread;
    std::string error;
    bool saw_auth_continue = false;
    std::string startup_database;

    void start() {
        thread = std::thread([this]() { run(); });
    }

    void stop() {
        if (listener) {
            listener->close();
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

private:
    void run() {
        scratchbird::core::ErrorContext ctx;
        scratchbird::network::NetworkAddress client_addr;
        auto client = listener->accept(&client_addr, &ctx);
        if (!client) {
            error = "accept failed: " + ctx.message;
            return;
        }

        uint8_t frame_type = 0;
        std::vector<uint8_t> frame_payload;
        if (!readManagerFrame(client.get(), frame_type, frame_payload, error)) {
            return;
        }
        if (frame_type != kMsgMcpHello) {
            error = "expected MCP_HELLO";
            return;
        }
        if (!sendManagerFrame(client.get(), kMsgStatusResponse, {}, error)) {
            return;
        }

        if (!readManagerFrame(client.get(), frame_type, frame_payload, error)) {
            return;
        }
        if (frame_type != kMsgMcpAuthStart) {
            error = "expected MCP_AUTH_START";
            return;
        }
        if (frame_payload.size() < 9) {
            error = "mcp auth_start payload truncated";
            return;
        }
        const uint32_t user_len = readU32Le(frame_payload.data());
        if (4 + user_len + 1 + 4 > frame_payload.size()) {
            error = "mcp auth_start username truncated";
            return;
        }
        size_t offset = 4 + user_len + 1;
        const uint32_t token_len = readU32Le(frame_payload.data() + offset);
        offset += 4;
        if (offset + token_len > frame_payload.size()) {
            error = "mcp auth_start token truncated";
            return;
        }
        const std::string token(
            reinterpret_cast<const char*>(frame_payload.data() + offset), token_len);
        if (!config.require_challenge && token != config.expected_auth_token) {
            error = "mcp auth_start token mismatch";
            return;
        }

        if (config.require_challenge) {
            if (!sendManagerFrame(client.get(), kMsgAuthChallenge, {}, error)) {
                return;
            }
            if (config.stop_after_auth_probe) {
                return;
            }
            if (!readManagerFrame(client.get(), frame_type, frame_payload, error)) {
                return;
            }
            if (frame_type != kMsgMcpAuthContinue) {
                error = "expected MCP_AUTH_CONTINUE";
                return;
            }
            saw_auth_continue = true;
            if (frame_payload.size() < 4) {
                error = "mcp auth_continue payload truncated";
                return;
            }
            const uint32_t continue_token_len = readU32Le(frame_payload.data());
            if (4 + continue_token_len > frame_payload.size()) {
                error = "mcp auth_continue token truncated";
                return;
            }
            const std::string continue_token(
                reinterpret_cast<const char*>(frame_payload.data() + 4), continue_token_len);
            if (continue_token != config.expected_auth_token) {
                error = "mcp auth_continue token mismatch";
                return;
            }
        }

        if (!sendManagerFrame(client.get(),
                              kMsgAuthResponse,
                              buildManagerAuthResponsePayload(!config.fail_auth,
                                                              "invalid manager token"),
                              error)) {
            return;
        }
        if (config.stop_after_auth_probe) {
            return;
        }
        if (config.fail_auth) {
            return;
        }

        if (!readManagerFrame(client.get(), frame_type, frame_payload, error)) {
            return;
        }
        if (frame_type != kMsgMcpDbConnect) {
            error = "expected MCP_DB_CONNECT";
            return;
        }

        if (!sendManagerFrame(client.get(),
                              kMsgConnectResponse,
                              buildManagerConnectResponsePayload(!config.fail_connect,
                                                                 "database connect denied"),
                              error)) {
            return;
        }
        if (config.fail_connect) {
            return;
        }

        scratchbird::protocol::ProtocolMessage startup;
        auto status = readMessage(client.get(), startup, &ctx);
        if (status != scratchbird::core::Status::OK) {
            error = "read startup failed: " + ctx.message;
            return;
        }
        if (startup.header.type != scratchbird::protocol::MessageType::Startup) {
            error = "expected startup message";
            return;
        }

        uint64_t features = 0;
        std::unordered_map<std::string, std::string> params;
        if (!parseStartupPayload(startup.body, features, params, error)) {
            return;
        }
        auto it = params.find("database");
        if (it != params.end()) {
            startup_database = it->second;
        }

        status = sendMessage(client.get(),
                             scratchbird::protocol::MessageType::AuthRequest,
                             buildAuthRequestPayload(scratchbird::protocol::AuthMethod::Ok),
                             1,
                             &ctx);
        if (status != scratchbird::core::Status::OK) {
            error = "write auth request failed: " + ctx.message;
            return;
        }
        status = sendMessage(client.get(),
                             scratchbird::protocol::MessageType::AuthOk,
                             buildAuthOkPayload(),
                             2,
                             &ctx);
        if (status != scratchbird::core::Status::OK) {
            error = "write auth ok failed: " + ctx.message;
            return;
        }
        status = sendMessage(client.get(),
                             scratchbird::protocol::MessageType::Ready,
                             buildReadyPayload(),
                             3,
                             &ctx);
        if (status != scratchbird::core::Status::OK) {
            error = "write ready failed: " + ctx.message;
            return;
        }
    }
};

void setupIpv4Listener(ServerHarness& harness) {
    harness.listener = scratchbird::network::Socket::create(
        scratchbird::network::AddressFamily::IPV4);
    ASSERT_TRUE(harness.listener);

    scratchbird::network::NetworkAddress addr("127.0.0.1", 0);
    scratchbird::core::ErrorContext ctx;
    auto status = harness.listener->bind(addr, &ctx);
    ASSERT_EQ(status, scratchbird::core::Status::OK) << ctx.message;

    status = harness.listener->listen();
    ASSERT_EQ(status, scratchbird::core::Status::OK);

    auto local = harness.listener->getLocalAddress();
    ASSERT_TRUE(local.has_value());
    harness.port = local->port;
    ASSERT_GT(harness.port, 0u);
}

void setupIpv4Listener(ManagerProxyHarness& harness) {
    harness.listener = scratchbird::network::Socket::create(
        scratchbird::network::AddressFamily::IPV4);
    ASSERT_TRUE(harness.listener);

    scratchbird::network::NetworkAddress addr("127.0.0.1", 0);
    scratchbird::core::ErrorContext ctx;
    auto status = harness.listener->bind(addr, &ctx);
    ASSERT_EQ(status, scratchbird::core::Status::OK) << ctx.message;

    status = harness.listener->listen();
    ASSERT_EQ(status, scratchbird::core::Status::OK);

    auto local = harness.listener->getLocalAddress();
    ASSERT_TRUE(local.has_value());
    harness.port = local->port;
    ASSERT_GT(harness.port, 0u);
}

#ifndef _WIN32
void setupUnixListener(ServerHarness& harness, std::string& path_out) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path socket_path =
        std::filesystem::temp_directory_path() /
        ("scratchbird_cpp_driver_" + std::to_string(now) + ".sock");
    std::error_code ec;
    std::filesystem::remove(socket_path, ec);

    harness.listener = scratchbird::network::Socket::create(
        scratchbird::network::AddressFamily::UNIX);
    ASSERT_TRUE(harness.listener);

    scratchbird::network::NetworkAddress addr(socket_path.string());
    scratchbird::core::ErrorContext ctx;
    auto status = harness.listener->bind(addr, &ctx);
    ASSERT_EQ(status, scratchbird::core::Status::OK) << ctx.message;

    status = harness.listener->listen();
    ASSERT_EQ(status, scratchbird::core::Status::OK);
    path_out = socket_path.string();
}
#endif

scratchbird::client::NetworkClientConfig makeLoopbackConfig(uint16_t port) {
    scratchbird::client::NetworkClientConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.ssl_mode = scratchbird::network::SSLMode::DISABLED;
    cfg.connect_timeout_ms = 2000;
    cfg.read_timeout_ms = 2000;
    cfg.write_timeout_ms = 2000;
    cfg.database = "main";
    cfg.autocommit = false;
    return cfg;
}

} // namespace

TEST(DriverConnectivitySmokeTest, ConnectsToLocalListener) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarness harness;
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClient client;
    scratchbird::client::NetworkClientConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = harness.port;
    cfg.ssl_mode = scratchbird::network::SSLMode::DISABLED;
    cfg.connect_timeout_ms = 2000;
    cfg.read_timeout_ms = 2000;
    cfg.write_timeout_ms = 2000;
    cfg.database = "default";
    scratchbird::core::ErrorContext ctx;

    auto status = client.connect(cfg, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    client.disconnect();

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverConnectivitySmokeTest, SslmodeDisableConnectsOnlyWhenPlaintextPolicyPermitsIt) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarness harness;
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClient client;
    auto cfg = makeLoopbackConfig(harness.port);
    cfg.ssl_mode = scratchbird::network::SSLMode::DISABLED;
    cfg.username = "alice";
    cfg.password = "scratchbird";

    scratchbird::core::ErrorContext ctx;
    auto status = client.connect(cfg, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    client.disconnect();

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
    EXPECT_EQ(harness.startup_params["database"], "main");
    EXPECT_EQ(harness.startup_params["user"], "alice");
}

TEST(DriverConnectivitySmokeTest, SslmodeDisableFailsWhenPlaintextPolicyRefusesStartup) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarness harness;
    setupIpv4Listener(harness);
    std::atomic<bool> accepted{false};
    std::thread policy_server([&harness, &accepted]() {
        scratchbird::core::ErrorContext ctx;
        scratchbird::network::NetworkAddress client_addr;
        auto client = harness.listener->accept(&client_addr, &ctx);
        if (client) {
            accepted.store(true, std::memory_order_relaxed);
            client->close();
        }
    });

    scratchbird::client::NetworkClient client;
    auto cfg = makeLoopbackConfig(harness.port);
    cfg.ssl_mode = scratchbird::network::SSLMode::DISABLED;
    cfg.connect_timeout_ms = 1000;
    cfg.read_timeout_ms = 1000;
    cfg.write_timeout_ms = 1000;

    scratchbird::core::ErrorContext ctx;
    const auto status = client.connect(cfg, &ctx);
    EXPECT_NE(status, scratchbird::core::Status::OK);
    EXPECT_TRUE(status == scratchbird::core::Status::CONNECTION_FAILURE ||
                status == scratchbird::core::Status::IO_ERROR ||
                status == scratchbird::core::Status::PROTOCOL_VIOLATION);
    client.disconnect();

    harness.listener->close();
    if (policy_server.joinable()) {
        policy_server.join();
    }
    EXPECT_TRUE(accepted.load(std::memory_order_relaxed));
}

TEST(DriverConnectivitySmokeTest, ConnectsWithPasswordAuthChallengeAndCarriesAuthParams) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarness harness(ServerHarnessConfig{
        scratchbird::protocol::AuthMethod::Password,
        "pw-secret"
    });
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClient client;
    scratchbird::client::NetworkClientConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = harness.port;
    cfg.ssl_mode = scratchbird::network::SSLMode::DISABLED;
    cfg.connect_timeout_ms = 2000;
    cfg.read_timeout_ms = 2000;
    cfg.write_timeout_ms = 2000;
    cfg.database = "main";
    cfg.username = "alice";
    cfg.password = "pw-secret";
    cfg.auth_method_id = "scratchbird.auth.proxy_assertion";
    cfg.auth_payload_json = "{\"subject\":\"alice\"}";
    cfg.auth_provider_profile = "corp_primary";
    cfg.auth_require_channel_binding = true;

    scratchbird::core::ErrorContext ctx;
    auto status = client.connect(cfg, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    client.disconnect();

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
    EXPECT_TRUE(harness.saw_auth_response);
    EXPECT_EQ(harness.startup_params["database"], "main");
    EXPECT_EQ(harness.startup_params["user"], "alice");
    EXPECT_EQ(harness.startup_params["auth_method_id"], "scratchbird.auth.proxy_assertion");
    EXPECT_EQ(harness.startup_params["auth_payload_json"], "{\"subject\":\"alice\"}");
    EXPECT_EQ(harness.startup_params["auth_provider_profile"], "corp_primary");
    EXPECT_EQ(harness.startup_params["auth_require_channel_binding"], "1");
    EXPECT_NE(harness.startup_features & scratchbird::protocol::kFeatureSblr, 0ULL);
    EXPECT_NE(harness.startup_features & scratchbird::protocol::kFeatureNotifications, 0ULL);
    EXPECT_NE(harness.startup_features & scratchbird::protocol::kFeatureQueryPlan, 0ULL);
}

TEST(DriverConnectivitySmokeTest, ProbeAuthSurfaceDirectReportsScramSha512) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarness harness(ServerHarnessConfig{
        scratchbird::protocol::AuthMethod::ScramSha512,
        "",
        false,
        false,
        true
    });
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClient client;
    auto cfg = makeLoopbackConfig(harness.port);
    cfg.username = "alice";

    scratchbird::client::AuthProbeResult result;
    scratchbird::core::ErrorContext ctx;
    auto status = client.probeAuthSurface(cfg, result, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::OK) << ctx.message;

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
    ASSERT_TRUE(result.reachable);
    EXPECT_EQ(result.ingress_mode, "direct");
    ASSERT_EQ(result.admitted_methods.size(), 1U);
    EXPECT_EQ(result.admitted_methods[0].wire_method, "SCRAM_SHA_512");
    EXPECT_EQ(result.required_method, "SCRAM_SHA_512");
    EXPECT_EQ(result.required_plugin_method_id, "scratchbird.auth.scram_sha_512");
    EXPECT_TRUE(result.admitted_methods[0].executable_locally);
    EXPECT_FALSE(result.admitted_methods[0].broker_required);
    EXPECT_TRUE(result.additional_continuation_possible);
}

TEST(DriverConnectivitySmokeTest, HandshakeSupportsScramSha512AndResolvedContext) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarness harness(ServerHarnessConfig{
        scratchbird::protocol::AuthMethod::Ok
    });
    setupIpv4Listener(harness);

    std::thread server([&harness]() {
        scratchbird::core::ErrorContext ctx;
        scratchbird::network::NetworkAddress client_addr;
        auto client = harness.listener->accept(&client_addr, &ctx);
        if (!client) {
            harness.error = "accept failed: " + ctx.message;
            return;
        }

        scratchbird::protocol::ProtocolMessage startup;
        auto status = readMessage(client.get(), startup, &ctx);
        if (status != scratchbird::core::Status::OK) {
            harness.error = "read startup failed: " + ctx.message;
            return;
        }

        status = sendMessage(client.get(),
                             scratchbird::protocol::MessageType::AuthRequest,
                             buildAuthRequestPayload(scratchbird::protocol::AuthMethod::ScramSha512),
                             1,
                             &ctx);
        if (status != scratchbird::core::Status::OK) {
            harness.error = "write auth request failed: " + ctx.message;
            return;
        }

        scratchbird::protocol::ProtocolMessage auth_first;
        status = readMessage(client.get(), auth_first, &ctx);
        if (status != scratchbird::core::Status::OK) {
            harness.error = "read scram client first failed: " + ctx.message;
            return;
        }
        const std::string client_first(auth_first.body.begin(), auth_first.body.end());
        const auto nonce_pos = client_first.find("r=");
        if (nonce_pos == std::string::npos) {
            harness.error = "scram client first missing nonce";
            return;
        }
        const std::string client_nonce = client_first.substr(nonce_pos + 2);
        const std::vector<uint8_t> salt = {'s', 'a', 'l', 't'};
        const std::string server_first =
            "r=" + client_nonce + "server,s=" + base64EncodeString(salt) + ",i=4096";

        status = sendMessage(client.get(),
                             scratchbird::protocol::MessageType::AuthContinue,
                             buildAuthContinuePayload(
                                 scratchbird::protocol::AuthMethod::ScramSha512,
                                 1,
                                 std::vector<uint8_t>(server_first.begin(), server_first.end())),
                             2,
                             &ctx);
        if (status != scratchbird::core::Status::OK) {
            harness.error = "write scram server first failed: " + ctx.message;
            return;
        }

        scratchbird::protocol::ProtocolMessage auth_final;
        status = readMessage(client.get(), auth_final, &ctx);
        if (status != scratchbird::core::Status::OK) {
            harness.error = "read scram client final failed: " + ctx.message;
            return;
        }

        status = sendMessage(client.get(),
                             scratchbird::protocol::MessageType::AuthOk,
                             buildAuthOkPayload(),
                             3,
                             &ctx);
        if (status != scratchbird::core::Status::OK) {
            harness.error = "write auth ok failed: " + ctx.message;
            return;
        }
        status = sendMessage(client.get(),
                             scratchbird::protocol::MessageType::Ready,
                             buildReadyPayload(),
                             4,
                             &ctx);
        if (status != scratchbird::core::Status::OK) {
            harness.error = "write ready failed: " + ctx.message;
            return;
        }
    });

    scratchbird::client::NetworkClient client;
    auto cfg = makeLoopbackConfig(harness.port);
    cfg.username = "alice";
    cfg.password = "pw-secret";

    scratchbird::core::ErrorContext ctx;
    auto status = client.connect(cfg, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    auto auth = client.getResolvedAuthContext();
    EXPECT_EQ(auth.ingress_mode, "direct");
    EXPECT_EQ(auth.resolved_auth_method, "SCRAM_SHA_512");
    EXPECT_EQ(auth.resolved_auth_plugin_id, "scratchbird.auth.scram_sha_512");
    EXPECT_FALSE(auth.manager_authenticated);
    EXPECT_TRUE(auth.attached);
    client.disconnect();

    if (server.joinable()) {
        server.join();
    }
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverConnectivitySmokeTest, HandshakeSupportsTokenAuth) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarness harness(ServerHarnessConfig{
        scratchbird::protocol::AuthMethod::Token,
        "bearer-token",
        true
    });
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClient client;
    auto cfg = makeLoopbackConfig(harness.port);
    cfg.username = "alice";
    cfg.auth_token = "bearer-token";

    scratchbird::core::ErrorContext ctx;
    auto status = client.connect(cfg, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    auto auth = client.getResolvedAuthContext();
    EXPECT_EQ(auth.resolved_auth_method, "TOKEN");
    EXPECT_EQ(auth.resolved_auth_plugin_id, "scratchbird.auth.authkey_token");
    EXPECT_TRUE(auth.attached);
    client.disconnect();

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
    EXPECT_TRUE(harness.saw_auth_response);
    EXPECT_EQ(harness.auth_response_count, 1U);
}

TEST(DriverConnectivitySmokeTest, HandshakePeerFailsClosed) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarness harness(ServerHarnessConfig{
        scratchbird::protocol::AuthMethod::Peer,
        "",
        false,
        false,
        true
    });
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClient client;
    auto cfg = makeLoopbackConfig(harness.port);
    cfg.username = "alice";

    scratchbird::core::ErrorContext ctx;
    auto status = client.connect(cfg, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::NOT_IMPLEMENTED);
    EXPECT_NE(ctx.message.find("not locally executable"), std::string::npos);

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverConnectivitySmokeTest, ConnectsWithCompressionCompatibilityParamsFromDsn) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarness harness;
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClientConfig cfg;
    scratchbird::core::ErrorContext ctx;
    const std::string conn_str = "scratchbird://alice:pw@127.0.0.1:" +
                                 std::to_string(harness.port) +
                                 "/main?sslmode=disable&autocommit=false&binary_transfer=false&compression=zstd";
    auto status = scratchbird::client::parseDriverConnectionString(conn_str, cfg, &ctx);
    ASSERT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    EXPECT_FALSE(cfg.binary_transfer);
    EXPECT_TRUE(cfg.enable_compression);

    scratchbird::client::NetworkClient client;
    status = client.connect(cfg, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    client.disconnect();

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
    EXPECT_NE(harness.startup_features & scratchbird::protocol::kFeatureCompression, 0ULL);
}

TEST(DriverCppApiClosureTest, HeaderHelpersExposeRetryBoundaryAndIsolationMeaning) {
    EXPECT_STREQ(sb_canonical_isolation_name(0), "READ COMMITTED");
    EXPECT_STREQ(sb_canonical_isolation_name(1), "READ COMMITTED");
    EXPECT_STREQ(sb_canonical_isolation_name(2), "SNAPSHOT");
    EXPECT_STREQ(sb_canonical_isolation_name(3), "SNAPSHOT TABLE STABILITY");
    EXPECT_STREQ(sb_canonical_isolation_name(99), "UNKNOWN");
    EXPECT_STREQ(sb_canonical_read_committed_mode_name(SB_READ_COMMITTED_MODE_DEFAULT), "DEFAULT");
    EXPECT_STREQ(sb_canonical_read_committed_mode_name(
                     SB_READ_COMMITTED_MODE_READ_CONSISTENCY),
                 "READ CONSISTENCY");
    EXPECT_STREQ(sb_canonical_read_committed_mode_name(
                     SB_READ_COMMITTED_MODE_RECORD_VERSION),
                 "RECORD VERSION");
    EXPECT_STREQ(sb_canonical_read_committed_mode_name(
                     SB_READ_COMMITTED_MODE_NO_RECORD_VERSION),
                 "NO RECORD VERSION");
    EXPECT_STREQ(sb_canonical_read_committed_mode_name(99), "UNKNOWN");

    EXPECT_EQ(sb_retry_scope_for_sqlstate("40001"), SB_RETRY_SCOPE_STATEMENT);
    EXPECT_EQ(sb_retry_scope_for_sqlstate("40P01"), SB_RETRY_SCOPE_STATEMENT);
    EXPECT_EQ(sb_retry_scope_for_sqlstate("08006"), SB_RETRY_SCOPE_RECONNECT);
    EXPECT_EQ(sb_retry_scope_for_sqlstate("57014"), SB_RETRY_SCOPE_NONE);
    EXPECT_EQ(sb_retry_scope_for_sqlstate(nullptr), SB_RETRY_SCOPE_NONE);
    EXPECT_TRUE(sb_is_retryable_sqlstate("40001"));
    EXPECT_FALSE(sb_is_retryable_sqlstate("57014"));
}

TEST(DriverCppApiClosureTest, CApiProbeAuthSurfaceJsonReportsDirectScramSha512) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarness harness(ServerHarnessConfig{
        scratchbird::protocol::AuthMethod::ScramSha512,
        "",
        false,
        false,
        true
    });
    setupIpv4Listener(harness);
    harness.start();

    const std::string conn_str =
        "scratchbird://alice:pw@127.0.0.1:" + std::to_string(harness.port) + "/main?sslmode=disable&autocommit=false";
    sb_error err{};
    char* json_text = sb_probe_auth_surface_json(conn_str.c_str(), &err);
    ASSERT_NE(json_text, nullptr) << err.message;
    auto parsed = nlohmann::json::parse(json_text);
    sb_memory_free(json_text);

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
    EXPECT_TRUE(parsed.at("reachable").get<bool>());
    EXPECT_EQ(parsed.at("ingress_mode").get<std::string>(), "direct");
    ASSERT_EQ(parsed.at("admitted_methods").size(), 1U);
    EXPECT_EQ(parsed.at("admitted_methods")[0].at("wire_method").get<std::string>(),
              "SCRAM_SHA_512");
}

TEST(DriverCppApiClosureTest, CApiResolvedAuthContextJsonReflectsConnectedSession) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarness harness(ServerHarnessConfig{
        scratchbird::protocol::AuthMethod::Password,
        "pw-secret"
    });
    setupIpv4Listener(harness);
    harness.start();

    const std::string conn_str =
        "scratchbird://alice:pw-secret@127.0.0.1:" + std::to_string(harness.port) +
        "/main?sslmode=disable&autocommit=false";
    sb_error err{};
    sb_connection* conn = sb_connect(conn_str.c_str(), &err);
    ASSERT_NE(conn, nullptr) << err.message;

    char* json_text = sb_get_resolved_auth_context_json(conn, &err);
    ASSERT_NE(json_text, nullptr) << err.message;
    auto parsed = nlohmann::json::parse(json_text);
    sb_memory_free(json_text);
    sb_disconnect(conn);

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
    EXPECT_EQ(parsed.at("ingress_mode").get<std::string>(), "direct");
    EXPECT_EQ(parsed.at("resolved_auth_method").get<std::string>(), "PASSWORD");
    EXPECT_EQ(parsed.at("resolved_auth_plugin_id").get<std::string>(),
              "scratchbird.auth.password_compat");
    EXPECT_TRUE(parsed.at("attached").get<bool>());
}

TEST(DriverConnectivitySmokeTest, RejectsLocalIpcWithoutPathBeforeDial) {
    scratchbird::client::NetworkClient client;
    scratchbird::client::NetworkClientConfig cfg;
    cfg.transport_mode = "local_ipc";
    cfg.ssl_mode = scratchbird::network::SSLMode::DISABLED;
    cfg.connect_timeout_ms = 250;
    cfg.read_timeout_ms = 250;
    cfg.write_timeout_ms = 250;
    cfg.database = "main";

    scratchbird::core::ErrorContext ctx;
    auto status = client.connect(cfg, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::INVALID_ARGUMENT);
    EXPECT_NE(ctx.message.find("ipc_path is required"), std::string::npos);
}

TEST(DriverConnectivitySmokeTest, ConnectionStringParseErrorsAreNotSilentlyRetriedAsDatabaseNames) {
    scratchbird::client::Connection conn;
    scratchbird::core::ErrorContext ctx;

    const auto status = conn.connect(
        "database=main;transport_mode=local_ipc;ipc_method=unknown",
        "alice",
        "pw",
        &ctx);

    EXPECT_EQ(status, scratchbird::core::Status::INVALID_ARGUMENT);
    EXPECT_NE(ctx.message.find("ipc_method"), std::string::npos);
}

TEST(DriverConnectivitySmokeTest, RejectsInvalidAuthMethodIdBeforeDial) {
    scratchbird::client::NetworkClient client;
    scratchbird::client::NetworkClientConfig cfg;
    cfg.host = "203.0.113.15";
    cfg.port = 6553;
    cfg.ssl_mode = scratchbird::network::SSLMode::DISABLED;
    cfg.database = "main";
    cfg.auth_method_id = "custom.namespace.token";

    scratchbird::core::ErrorContext ctx;
    auto status = client.connect(cfg, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::INVALID_ARGUMENT);
    EXPECT_NE(ctx.message.find("auth_method_id"), std::string::npos);
}

TEST(DriverConnectivitySmokeTest, RejectsManagerProxyModeWithoutTokenBeforeDial) {
    scratchbird::client::NetworkClient client;
    scratchbird::client::NetworkClientConfig cfg;
    cfg.transport_mode = "managed";
    cfg.host = "203.0.113.15";
    cfg.port = 6553;
    cfg.ssl_mode = scratchbird::network::SSLMode::DISABLED;
    cfg.database = "main";

    scratchbird::core::ErrorContext ctx;
    auto status = client.connect(cfg, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::INVALID_ARGUMENT);
    EXPECT_NE(ctx.message.find("manager_auth_token"), std::string::npos);
}

TEST(DriverConnectivitySmokeTest, ProbeAuthSurfaceManagerProxyReportsToken) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ManagerProxyHarness harness(ManagerProxyHarnessConfig{
        false,
        false,
        true,
        true,
        "manager-token"
    });
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClient client;
    auto cfg = makeLoopbackConfig(harness.port);
    cfg.transport_mode = "managed";
    cfg.front_door_mode = "manager_proxy";
    cfg.username = "alice";

    scratchbird::client::AuthProbeResult result;
    scratchbird::core::ErrorContext ctx;
    auto status = client.probeAuthSurface(cfg, result, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::OK) << ctx.message;

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
    ASSERT_TRUE(result.reachable);
    EXPECT_EQ(result.ingress_mode, "manager_proxy");
    ASSERT_EQ(result.admitted_methods.size(), 1U);
    EXPECT_EQ(result.admitted_methods[0].wire_method, "TOKEN");
    EXPECT_EQ(result.required_method, "TOKEN");
    EXPECT_EQ(result.required_plugin_method_id, "scratchbird.auth.authkey_token");
    EXPECT_TRUE(result.admitted_methods[0].executable_locally);
    EXPECT_FALSE(result.admitted_methods[0].broker_required);
    EXPECT_TRUE(result.additional_continuation_possible);
}

TEST(DriverConnectivitySmokeTest, ConnectsThroughManagerProxyHandshake) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ManagerProxyHarness harness(ManagerProxyHarnessConfig{
        false,
        false,
        true,
        false,
        "manager-token"
    });
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClient client;
    auto cfg = makeLoopbackConfig(harness.port);
    cfg.transport_mode = "managed";
    cfg.front_door_mode = "manager_proxy";
    cfg.manager_auth_token = "manager-token";
    cfg.manager_auth_fast_path = false;
    cfg.database = "control";

    scratchbird::core::ErrorContext ctx;
    auto status = client.connect(cfg, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    client.disconnect();

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
    EXPECT_TRUE(harness.saw_auth_continue);
    EXPECT_EQ(harness.startup_database, "control");
}

TEST(DriverConnectivitySmokeTest, ManagerProxyAuthFailureMapsToInvalidAuthorization) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ManagerProxyHarness harness(ManagerProxyHarnessConfig{
        true,
        false,
        false,
        false,
        "bad-token"
    });
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClient client;
    auto cfg = makeLoopbackConfig(harness.port);
    cfg.transport_mode = "managed";
    cfg.front_door_mode = "manager_proxy";
    cfg.manager_auth_token = "bad-token";
    cfg.database = "main";

    scratchbird::core::ErrorContext ctx;
    auto status = client.connect(cfg, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::INVALID_AUTHORIZATION);
    EXPECT_NE(ctx.message.find("invalid manager token"), std::string::npos);

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, TransactionRoundTripBeginCommitRollback) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::TxnCommit,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 0, 0, "COMMIT")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 2, 2)}},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             scratchbird::protocol::TxnCommitRequest request;
             scratchbird::core::ErrorContext parse_ctx;
             if (scratchbird::protocol::parseTxnCommitRequest(msg.body, request, &parse_ctx) !=
                 scratchbird::core::Status::OK) {
                 error = "txn commit payload parse failed: " + parse_ctx.message;
                 return false;
             }
             if (msg.body.size() != 36) {
                 error = "txn commit finality payload size mismatch";
                 return false;
             }
             if ((request.contract_flags &
                  scratchbird::protocol::kTxnCommitFlagHasIdempotencyKey) == 0 ||
                 (request.contract_flags &
                  scratchbird::protocol::kTxnCommitFlagStatementHasSideEffects) == 0 ||
                 isZeroUuid(request.idempotency_key)) {
                 error = "txn commit idempotency contract missing";
                 return false;
             }
             if (request.expected_txn_id != 1) {
                 error = "txn commit expected transaction mismatch";
                 return false;
             }
             return true;
         }},
        {scratchbird::protocol::MessageType::TxnRollback,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 0, 0, "ROLLBACK")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 4, 4)}}}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClient client;
    auto cfg = makeLoopbackConfig(harness.port);
    scratchbird::core::ErrorContext ctx;
    ASSERT_EQ(client.connect(cfg, &ctx), scratchbird::core::Status::OK) << ctx.message;

    EXPECT_EQ(client.beginTransaction(&ctx), scratchbird::core::Status::OK) << ctx.message;
    EXPECT_EQ(client.commit(&ctx), scratchbird::core::Status::OK) << ctx.message;
    EXPECT_EQ(client.beginTransaction(&ctx), scratchbird::core::Status::OK) << ctx.message;
    EXPECT_EQ(client.rollback(&ctx), scratchbird::core::Status::OK) << ctx.message;
    client.disconnect();

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, CommitLostReadyLeavesUnknownFinalityAndRefusesRetry) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    std::array<uint8_t, 16> captured_key{};
    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::TxnCommit,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 0, 0, "COMMIT")}},
         [&captured_key](const scratchbird::protocol::ProtocolMessage& msg,
                         std::string& error) {
             scratchbird::protocol::TxnCommitRequest request;
             scratchbird::core::ErrorContext parse_ctx;
             if (scratchbird::protocol::parseTxnCommitRequest(msg.body, request, &parse_ctx) !=
                 scratchbird::core::Status::OK) {
                 error = "txn commit payload parse failed: " + parse_ctx.message;
                 return false;
             }
             captured_key = request.idempotency_key;
             if (isZeroUuid(captured_key)) {
                 error = "commit idempotency key was zero";
                 return false;
             }
             return true;
         }}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClient client;
    auto cfg = makeLoopbackConfig(harness.port);
    scratchbird::core::ErrorContext ctx;
    ASSERT_EQ(client.connect(cfg, &ctx), scratchbird::core::Status::OK) << ctx.message;

    scratchbird::core::ErrorContext commit_ctx;
    EXPECT_NE(client.commit(&commit_ctx), scratchbird::core::Status::OK);
    const auto& finality = client.lastCommitFinality();
    EXPECT_EQ(finality.state, scratchbird::protocol::TxnFinalityState::Unknown);
    EXPECT_FALSE(finality.engineFinalityKnown());
    EXPECT_EQ(finality.idempotency_key, captured_key);
    EXPECT_EQ(finality.diagnostic_code, "SBWP.COMMIT.READY_LOST_FINALITY_UNKNOWN");

    scratchbird::core::ErrorContext retry_ctx;
    EXPECT_EQ(client.validateRetryAfterCommitUncertainty(captured_key, true, true, &retry_ctx),
              scratchbird::core::Status::CONNECTION_FAILURE);
    EXPECT_STREQ(retry_ctx.sqlstate, "08007");

    client.disconnect();
    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, CommitFinalityQueryMakesSameKeyReplayableAndDifferentKeyRefused) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    std::array<uint8_t, 16> captured_key{};
    std::array<uint8_t, 16> finality_token = testUuid(0xA0);
    uint64_t captured_fingerprint = 0;
    bool saw_query = false;

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::TxnCommit,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 0, 0, "COMMIT")},
          {scratchbird::protocol::MessageType::TxnStatus,
           {},
           [&captured_key, &captured_fingerprint, &finality_token](
               const scratchbird::protocol::ProtocolMessage&) {
               return buildTxnFinalityStatusPayload(
                   scratchbird::protocol::TxnFinalityState::Unknown,
                   0,
                   captured_key,
                   finality_token,
                   captured_fingerprint,
                   1,
                   0,
                   "SBWP.COMMIT.FINALITY_UNKNOWN",
                   "unknown_until_engine_finality_report");
           }},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 2, 2)}},
         [&captured_key, &captured_fingerprint](
             const scratchbird::protocol::ProtocolMessage& msg,
             std::string& error) {
             scratchbird::protocol::TxnCommitRequest request;
             scratchbird::core::ErrorContext parse_ctx;
             if (scratchbird::protocol::parseTxnCommitRequest(msg.body, request, &parse_ctx) !=
                 scratchbird::core::Status::OK) {
                 error = "txn commit payload parse failed: " + parse_ctx.message;
                 return false;
             }
             if ((request.contract_flags &
                  scratchbird::protocol::kTxnCommitFlagHasIdempotencyKey) == 0 ||
                 (request.contract_flags &
                  scratchbird::protocol::kTxnCommitFlagStatementHasSideEffects) == 0 ||
                 isZeroUuid(request.idempotency_key)) {
                 error = "commit idempotency contract missing";
                 return false;
             }
             captured_key = request.idempotency_key;
             captured_fingerprint = request.request_fingerprint;
             return true;
         }},
        {scratchbird::protocol::MessageType::TxnStatus,
         {{scratchbird::protocol::MessageType::TxnStatus,
           {},
           [&captured_key, &captured_fingerprint, &finality_token](
               const scratchbird::protocol::ProtocolMessage&) {
               return buildTxnFinalityStatusPayload(
                   scratchbird::protocol::TxnFinalityState::Committed,
                   scratchbird::protocol::kTxnFinalityFlagEngineKnown |
                       scratchbird::protocol::kTxnFinalityFlagSameIdempotencyKeyReplayable,
                   captured_key,
                   finality_token,
                   captured_fingerprint,
                   1,
                   2,
                   "SBWP.COMMIT.FINALITY_COMMITTED_BY_MGA_INVENTORY",
                   "committed_by_engine_mga_inventory");
           }},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 2, 2)}},
         [&captured_key, &finality_token, &saw_query](
             const scratchbird::protocol::ProtocolMessage& msg,
             std::string& error) {
             scratchbird::protocol::TxnFinalityQuery query;
             scratchbird::core::ErrorContext parse_ctx;
             if (scratchbird::protocol::parseTxnFinalityQuery(msg.body, query, &parse_ctx) !=
                 scratchbird::core::Status::OK) {
                 error = "txn finality query parse failed: " + parse_ctx.message;
                 return false;
             }
             if (query.idempotency_key != captured_key) {
                 error = "finality query idempotency key mismatch";
                 return false;
             }
             if (query.finality_token != finality_token) {
                 error = "finality query token mismatch";
                 return false;
             }
             saw_query = true;
             return true;
         }}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClient client;
    auto cfg = makeLoopbackConfig(harness.port);
    scratchbird::core::ErrorContext ctx;
    ASSERT_EQ(client.connect(cfg, &ctx), scratchbird::core::Status::OK) << ctx.message;

    ASSERT_EQ(client.commit(&ctx), scratchbird::core::Status::OK) << ctx.message;
    EXPECT_EQ(client.lastCommitFinality().state,
              scratchbird::protocol::TxnFinalityState::Unknown);
    EXPECT_FALSE(client.lastCommitFinality().engineFinalityKnown());

    scratchbird::core::ErrorContext retry_before_query_ctx;
    EXPECT_EQ(client.validateRetryAfterCommitUncertainty(captured_key,
                                                         true,
                                                         true,
                                                         &retry_before_query_ctx),
              scratchbird::core::Status::CONNECTION_FAILURE);
    EXPECT_STREQ(retry_before_query_ctx.sqlstate, "08007");

    ASSERT_EQ(client.queryLastCommitFinality(&ctx), scratchbird::core::Status::OK)
        << ctx.message;
    EXPECT_TRUE(saw_query);
    const auto& finality = client.lastCommitFinality();
    EXPECT_EQ(finality.state, scratchbird::protocol::TxnFinalityState::Committed);
    EXPECT_TRUE(finality.engineFinalityKnown());
    EXPECT_TRUE(finality.sameIdempotencyKeyReplayable());
    EXPECT_EQ(finality.idempotency_key, captured_key);
    EXPECT_EQ(finality.finality_token, finality_token);
    EXPECT_EQ(finality.request_fingerprint, captured_fingerprint);
    EXPECT_EQ(finality.original_txn_id, 1u);
    EXPECT_EQ(finality.replacement_txn_id, 2u);
    EXPECT_EQ(finality.diagnostic_code,
              "SBWP.COMMIT.FINALITY_COMMITTED_BY_MGA_INVENTORY");

    scratchbird::core::ErrorContext side_effect_ctx;
    EXPECT_EQ(client.validateRetryAfterCommitUncertainty(captured_key,
                                                         true,
                                                         false,
                                                         &side_effect_ctx),
              scratchbird::core::Status::INVALID_TRANSACTION_STATE);
    EXPECT_STREQ(side_effect_ctx.sqlstate, "40003");

    scratchbird::core::ErrorContext acknowledged_ctx;
    EXPECT_EQ(client.validateRetryAfterCommitUncertainty(captured_key,
                                                         true,
                                                         true,
                                                         &acknowledged_ctx),
              scratchbird::core::Status::OK);

    scratchbird::core::ErrorContext different_key_ctx;
    EXPECT_EQ(client.validateRetryAfterCommitUncertainty(testUuid(0xB0),
                                                         false,
                                                         true,
                                                         &different_key_ctx),
              scratchbird::core::Status::INVALID_TRANSACTION_STATE);
    EXPECT_STREQ(different_key_ctx.sqlstate, "40003");

    client.disconnect();
    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, CApiTxnBeginExEncodesEnterpriseOptions) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::TxnBegin,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 0, 0, "BEGIN")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 50, 1)}},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             uint16_t flags = 0;
             uint8_t conflict_action = 0;
             uint8_t autocommit_mode = 0;
             uint8_t isolation_level = 0;
             uint8_t read_committed_mode = 0;
             uint8_t access_mode = 0;
             uint8_t deferrable = 0;
             uint8_t wait_mode = 0;
             uint32_t timeout_ms = 0;
             if (!parseTxnBeginPayload(msg.body,
                                       flags,
                                       conflict_action,
                                       autocommit_mode,
                                       isolation_level,
                                       read_committed_mode,
                                       access_mode,
                                       deferrable,
                                       wait_mode,
                                       timeout_ms,
                                       error)) {
                 return false;
             }
             return flags == 0x0107 &&
                    conflict_action == 2 &&
                    autocommit_mode == 1 &&
                    isolation_level == 3 &&
                    read_committed_mode ==
                        SB_READ_COMMITTED_MODE_READ_CONSISTENCY &&
                    access_mode == 1 &&
                    deferrable == 1 &&
                    wait_mode == 1 &&
                    timeout_ms == 2500;
         }}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    const std::string conn_str = "scratchbird://127.0.0.1:" + std::to_string(harness.port) +
                                 "/main?sslmode=disable&autocommit=false";
    sb_error err{};
    sb_connection* conn = sb_connect(conn_str.c_str(), &err);
    ASSERT_NE(conn, nullptr) << err.message;

    sb_txn_options options{};
    options.flags = 0x07;
    options.conflict_action = 2;
    options.autocommit_mode = 1;
    options.isolation_level = 3;
    options.read_committed_mode = SB_READ_COMMITTED_MODE_READ_CONSISTENCY;
    options.access_mode = 1;
    options.deferrable = 1;
    options.wait_mode = 1;
    options.timeout_ms = 2500;
    EXPECT_EQ(sb_tx_begin_ex(conn, &options, &err), SB_OK) << err.message;

    sb_disconnect(conn);
    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, CApiNotificationQueueAndListenerEnterpriseSurface) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::Subscribe,
         {{scratchbird::protocol::MessageType::Notification,
           buildNotificationPayload(81, "events", std::vector<uint8_t>{'h', 'i'}, 'U', 77, true)},
          {scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 0, 0, "SUBSCRIBE")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 1, 1)}}},
        {scratchbird::protocol::MessageType::Ping,
         {{scratchbird::protocol::MessageType::Notification,
           buildNotificationPayload(82, "events", std::vector<uint8_t>{'o', 'k'}, 'I', 88, true)},
          {scratchbird::protocol::MessageType::Pong, {}}}}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    const std::string conn_str = "scratchbird://127.0.0.1:" + std::to_string(harness.port) +
                                 "/main?sslmode=disable&autocommit=false";
    sb_error err{};
    sb_connection* conn = sb_connect(conn_str.c_str(), &err);
    ASSERT_NE(conn, nullptr) << err.message;

    EXPECT_EQ(sb_listen(conn, "events", "", &err), SB_OK) << err.message;
    EXPECT_EQ(sb_notification_count(conn), static_cast<size_t>(1));

    auto first_json_raw = sb_get_notification(conn, &err);
    ASSERT_NE(first_json_raw, nullptr);
    const auto first_json = nlohmann::json::parse(first_json_raw);
    sb_memory_free(first_json_raw);
    EXPECT_EQ(first_json["channel"], "events");
    EXPECT_EQ(first_json["payload_hex"], "6869");
    EXPECT_EQ(first_json["row_id"], 77);

    NotificationProbe probe;
    const uint64_t listener_id = sb_add_notification_listener(conn, notificationProbeCallback, &probe, &err);
    ASSERT_NE(listener_id, 0u) << err.message;

    EXPECT_EQ(sb_ping(conn, &err), SB_OK) << err.message;
    EXPECT_EQ(probe.calls, 1);
    EXPECT_EQ(probe.channel, "events");
    EXPECT_EQ(probe.payload_hex, "6f6b");
    EXPECT_EQ(probe.row_id, 88u);

    auto queue_json_raw = sb_get_notifications(conn, &err);
    ASSERT_NE(queue_json_raw, nullptr);
    const auto queue_json = nlohmann::json::parse(queue_json_raw);
    sb_memory_free(queue_json_raw);
    ASSERT_EQ(queue_json.size(), 1u);
    EXPECT_EQ(queue_json[0]["payload_hex"], "6f6b");

    EXPECT_EQ(sb_remove_notification_listener(conn, listener_id, &err), SB_OK) << err.message;
    sb_disconnect(conn);
    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, CApiDiagnosticsAndTelemetrySummaries) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::Query,
         {{scratchbird::protocol::MessageType::RowDescription,
           buildRowDescriptionPayload({{"v", scratchbird::protocol::kOidInt4}})},
          {scratchbird::protocol::MessageType::DataRow, buildDataRowPayload({encodeI32Le(1)})},
          {scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 1, 0, "SELECT 1")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 1, 1)}}}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    const std::string conn_str = "scratchbird://127.0.0.1:" + std::to_string(harness.port) +
                                 "/main?sslmode=disable&autocommit=false";
    sb_error err{};
    sb_connection* conn = sb_connect(conn_str.c_str(), &err);
    ASSERT_NE(conn, nullptr) << err.message;

    sb_result* result = sb_execute(conn, "SELECT 1", &err);
    ASSERT_NE(result, nullptr) << err.message;
    sb_result_free(result);

    auto telemetry_raw = sb_get_telemetry_summary_json(conn, &err);
    ASSERT_NE(telemetry_raw, nullptr);
    const auto telemetry = nlohmann::json::parse(telemetry_raw);
    sb_memory_free(telemetry_raw);
    EXPECT_GE(telemetry["total_invocations"].get<int64_t>(), 1);

    auto diagnostics_raw = sb_get_diagnostics_json(conn, &err);
    ASSERT_NE(diagnostics_raw, nullptr);
    const auto diagnostics = nlohmann::json::parse(diagnostics_raw);
    sb_memory_free(diagnostics_raw);
    EXPECT_EQ(diagnostics["database"], "main");
    EXPECT_TRUE(diagnostics.contains("notification_queue_depth"));

    EXPECT_EQ(sb_reset_telemetry(conn, &err), SB_OK) << err.message;
    auto telemetry_reset_raw = sb_get_telemetry_summary_json(conn, &err);
    ASSERT_NE(telemetry_reset_raw, nullptr);
    const auto telemetry_reset = nlohmann::json::parse(telemetry_reset_raw);
    sb_memory_free(telemetry_reset_raw);
    EXPECT_EQ(telemetry_reset["total_invocations"], 0);

    sb_disconnect(conn);
    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, SavepointRoundTripUsesTxnMessages) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::TxnSavepoint,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 0, 0, "SAVEPOINT")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 100, 2)}},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string savepoint;
             if (!parseTxnNamePayload(msg.body, savepoint, error)) {
                 return false;
             }
             if (savepoint != "sp1") {
                 error = "unexpected savepoint name";
                 return false;
             }
             return true;
         }},
        {scratchbird::protocol::MessageType::TxnRollbackTo,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 0, 0, "ROLLBACK TO SAVEPOINT")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 100, 3)}},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string savepoint;
             if (!parseTxnNamePayload(msg.body, savepoint, error)) {
                 return false;
             }
             if (savepoint != "sp1") {
                 error = "unexpected savepoint name";
                 return false;
             }
             return true;
         }},
        {scratchbird::protocol::MessageType::TxnRelease,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 0, 0, "RELEASE SAVEPOINT")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 100, 4)}},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string savepoint;
             if (!parseTxnNamePayload(msg.body, savepoint, error)) {
                 return false;
             }
             if (savepoint != "sp1") {
                 error = "unexpected savepoint name";
                 return false;
             }
             return true;
         }},
        {scratchbird::protocol::MessageType::TxnRollback,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 0, 0, "ROLLBACK")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 5, 5)}}}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClient client;
    auto cfg = makeLoopbackConfig(harness.port);
    scratchbird::core::ErrorContext ctx;
    ASSERT_EQ(client.connect(cfg, &ctx), scratchbird::core::Status::OK) << ctx.message;

    EXPECT_EQ(client.beginTransaction(&ctx), scratchbird::core::Status::OK) << ctx.message;
    EXPECT_EQ(client.savepoint("sp1", &ctx), scratchbird::core::Status::OK) << ctx.message;
    EXPECT_EQ(client.rollbackToSavepoint("sp1", &ctx), scratchbird::core::Status::OK) << ctx.message;
    EXPECT_EQ(client.releaseSavepoint("sp1", &ctx), scratchbird::core::Status::OK) << ctx.message;
    EXPECT_EQ(client.rollback(&ctx), scratchbird::core::Status::OK) << ctx.message;
    client.disconnect();

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, RollbackMapsNoActiveTransactionSqlState) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::TxnRollback,
         {{scratchbird::protocol::MessageType::Error,
           buildErrorPayload("ERROR", "25P01", "no active transaction")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 1, 1)}}}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClient client;
    auto cfg = makeLoopbackConfig(harness.port);
    scratchbird::core::ErrorContext ctx;
    ASSERT_EQ(client.connect(cfg, &ctx), scratchbird::core::Status::OK) << ctx.message;

    auto status = client.rollback(&ctx);
    EXPECT_EQ(status, scratchbird::core::Status::NO_ACTIVE_TRANSACTION);
    EXPECT_STREQ(ctx.sqlstate, "25P01");
    EXPECT_NE(ctx.message.find("no active transaction"), std::string::npos);
    client.disconnect();

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, CommitMapsReadOnlyAndAbortedSqlStates) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::TxnCommit,
         {{scratchbird::protocol::MessageType::Error,
           buildErrorPayload("ERROR", "25006", "read only transaction")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 2, 2)}}},
        {scratchbird::protocol::MessageType::TxnCommit,
         {{scratchbird::protocol::MessageType::Error,
           buildErrorPayload("ERROR", "25P02", "transaction is aborted")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 3, 3)}}}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClient client;
    auto cfg = makeLoopbackConfig(harness.port);
    scratchbird::core::ErrorContext ctx;
    ASSERT_EQ(client.connect(cfg, &ctx), scratchbird::core::Status::OK) << ctx.message;

    auto status = client.commit(&ctx);
    EXPECT_EQ(status, scratchbird::core::Status::READ_ONLY_TRANSACTION);
    EXPECT_STREQ(ctx.sqlstate, "25006");

    status = client.commit(&ctx);
    EXPECT_EQ(status, scratchbird::core::Status::TRANSACTION_ABORTED);
    EXPECT_STREQ(ctx.sqlstate, "25P02");
    client.disconnect();

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, QueryClearsCancelSequenceAfterReady) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.drain_after_script = true;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::Query,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 3, 0, "UPDATE 3")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 10, 10)}},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string sql;
             if (!parseQueryPayloadSql(msg.body, sql, error)) {
                 return false;
             }
             if (sql != "UPDATE t SET v = 1") {
                 error = "unexpected query sql";
                 return false;
             }
             return true;
         }}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClient client;
    auto cfg = makeLoopbackConfig(harness.port);
    scratchbird::core::ErrorContext ctx;
    ASSERT_EQ(client.connect(cfg, &ctx), scratchbird::core::Status::OK) << ctx.message;

    scratchbird::client::NetworkResultSet results;
    auto status = client.executeQuery("UPDATE t SET v = 1", results, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    EXPECT_EQ(results.rows_affected, 3);
    EXPECT_EQ(results.command_tag, "UPDATE 3");

    scratchbird::core::ErrorContext cancel_ctx;
    auto cancel_status = client.sendQueryCancel(&cancel_ctx);
    EXPECT_EQ(cancel_status, scratchbird::core::Status::INVALID_ARGUMENT);
    EXPECT_NE(cancel_ctx.message.find("No in-flight query to cancel"), std::string::npos);
    client.disconnect();

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, CancelDuringInFlightQueryUsesCancelMessage) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::Query,
         {},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string sql;
             if (!parseQueryPayloadSql(msg.body, sql, error)) {
                 return false;
             }
             if (sql != "SELECT pg_sleep(10)") {
                 error = "unexpected query sql";
                 return false;
             }
             return true;
         }},
        {scratchbird::protocol::MessageType::Cancel,
         {{scratchbird::protocol::MessageType::Error,
           buildErrorPayload("ERROR", "57014", "query canceled")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 11, 11)}},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             uint32_t cancel_type = 0;
             uint32_t sequence = 0;
             if (!parseCancelPayload(msg.body, cancel_type, sequence, error)) {
                 return false;
             }
             if (cancel_type != 0) {
                 error = "unexpected cancel type";
                 return false;
             }
             if (sequence == 0) {
                 error = "cancel sequence must be non-zero";
                 return false;
             }
             return true;
         }}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClient client;
    auto cfg = makeLoopbackConfig(harness.port);
    scratchbird::core::ErrorContext ctx;
    ASSERT_EQ(client.connect(cfg, &ctx), scratchbird::core::Status::OK) << ctx.message;

    std::atomic<scratchbird::core::Status> query_status{scratchbird::core::Status::OK};
    std::thread worker([&]() {
        scratchbird::client::NetworkResultSet ignored;
        scratchbird::core::ErrorContext worker_ctx;
        query_status = client.executeQuery("SELECT pg_sleep(10)", ignored, &worker_ctx);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    scratchbird::core::ErrorContext cancel_ctx;
    const auto cancel_status = client.sendQueryCancel(&cancel_ctx);
    EXPECT_EQ(cancel_status, scratchbird::core::Status::OK) << cancel_ctx.message;

    worker.join();
    EXPECT_EQ(query_status.load(), scratchbird::core::Status::QUERY_CANCELED);
    client.disconnect();

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, PrepareAndExecutePreparedRoundTrip) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::Parse, {}},
        {scratchbird::protocol::MessageType::Describe, {}},
        {scratchbird::protocol::MessageType::Sync,
         {{scratchbird::protocol::MessageType::ParameterDescription,
           buildParameterDescriptionPayload({scratchbird::protocol::kOidInt4})},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 20, 20)}}},
        {scratchbird::protocol::MessageType::Bind, {}},
        {scratchbird::protocol::MessageType::Execute,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 1, 0, "UPDATE 1")}}},
        {scratchbird::protocol::MessageType::Sync,
         {{scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 21, 21)}}}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClient client;
    auto cfg = makeLoopbackConfig(harness.port);
    scratchbird::core::ErrorContext ctx;
    ASSERT_EQ(client.connect(cfg, &ctx), scratchbird::core::Status::OK) << ctx.message;

    scratchbird::client::NetworkPreparedStatement stmt;
    auto status = client.prepare("UPDATE t SET v = $1", stmt, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    EXPECT_TRUE(stmt.isValid());
    EXPECT_EQ(stmt.getParameterCount(), 1u);
    stmt.setInt32(1, 7);

    scratchbird::client::NetworkResultSet results;
    status = client.executePrepared(stmt, results, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    EXPECT_EQ(results.rows_affected, 1);
    EXPECT_EQ(results.command_tag, "UPDATE 1");
    client.disconnect();

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, QuestionPlaceholderPrepareReparsesWithBoundTypes) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    constexpr const char* kSql = "INSERT INTO users.public.t(id, name) VALUES (?, ?)";

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::Parse,
         {},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string statement;
             std::string sql;
             std::vector<uint32_t> types;
             if (!parseParsePayloadStatementAndSql(msg.body, statement, sql, types, error)) {
                 return false;
             }
             if (sql != kSql) {
                 error = "unexpected initial prepared SQL";
                 return false;
             }
             if (types != std::vector<uint32_t>({0, 0})) {
                 error = "initial PARSE did not advertise two unknown parameters";
                 return false;
             }
             return true;
         }},
        {scratchbird::protocol::MessageType::Describe, {}},
        {scratchbird::protocol::MessageType::Sync,
         {{scratchbird::protocol::MessageType::ParameterDescription,
           buildParameterDescriptionPayload({0, 0})},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 20, 20)}}},
        {scratchbird::protocol::MessageType::Parse,
         {},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string statement;
             std::string sql;
             std::vector<uint32_t> types;
             if (!parseParsePayloadStatementAndSql(msg.body, statement, sql, types, error)) {
                 return false;
             }
             if (sql != kSql) {
                 error = "unexpected typed prepared SQL";
                 return false;
             }
             if (types != std::vector<uint32_t>(
                              {scratchbird::protocol::kOidInt4,
                               scratchbird::protocol::kOidText})) {
                 error = "typed PARSE did not use bound parameter OIDs";
                 return false;
             }
             return true;
         }},
        {scratchbird::protocol::MessageType::Describe, {}},
        {scratchbird::protocol::MessageType::Sync,
         {{scratchbird::protocol::MessageType::ParameterDescription,
           buildParameterDescriptionPayload({scratchbird::protocol::kOidInt4,
                                             scratchbird::protocol::kOidText})},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 21, 21)}}},
        {scratchbird::protocol::MessageType::Bind, {}},
        {scratchbird::protocol::MessageType::Execute,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 1, 0, "INSERT 1")}}},
        {scratchbird::protocol::MessageType::Sync,
         {{scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 22, 22)}}}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClient client;
    auto cfg = makeLoopbackConfig(harness.port);
    scratchbird::core::ErrorContext ctx;
    ASSERT_EQ(client.connect(cfg, &ctx), scratchbird::core::Status::OK) << ctx.message;

    scratchbird::client::NetworkPreparedStatement stmt;
    auto status = client.prepare(kSql, stmt, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    EXPECT_TRUE(stmt.isValid());
    EXPECT_EQ(stmt.getParameterCount(), 2u);
    stmt.setInt32(1, 7);
    stmt.setString(2, "alpha");

    scratchbird::client::NetworkResultSet results;
    status = client.executePrepared(stmt, results, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    EXPECT_EQ(results.rows_affected, 1);
    EXPECT_EQ(results.command_tag, "INSERT 1");
    client.disconnect();

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, ExecuteServerStatementAndCloseRoundTrip) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::Parse, {}},
        {scratchbird::protocol::MessageType::Describe, {}},
        {scratchbird::protocol::MessageType::Sync,
         {{scratchbird::protocol::MessageType::ParameterDescription,
           buildParameterDescriptionPayload({scratchbird::protocol::kOidInt4})},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 30, 30)}}},
        {scratchbird::protocol::MessageType::Bind, {}},
        {scratchbird::protocol::MessageType::Execute,
         {{scratchbird::protocol::MessageType::RowDescription,
           buildRowDescriptionPayload({{"v", scratchbird::protocol::kOidInt4}})},
          {scratchbird::protocol::MessageType::DataRow,
           buildDataRowPayload({encodeI32Le(7)})},
          {scratchbird::protocol::MessageType::PortalSuspended, {}}}},
        {scratchbird::protocol::MessageType::Sync,
         {{scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 31, 31)}}},
        {scratchbird::protocol::MessageType::Close,
         {{scratchbird::protocol::MessageType::CloseComplete, {}}}},
        {scratchbird::protocol::MessageType::Sync,
         {{scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 32, 32)}}}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClient client;
    auto cfg = makeLoopbackConfig(harness.port);
    scratchbird::core::ErrorContext ctx;
    ASSERT_EQ(client.connect(cfg, &ctx), scratchbird::core::Status::OK) << ctx.message;

    uint32_t statement_id = 0;
    ASSERT_EQ(client.prepareServerStatement("SELECT $1", statement_id, &ctx),
              scratchbird::core::Status::OK) << ctx.message;
    EXPECT_GT(statement_id, 0u);

    scratchbird::protocol::ParamValue param;
    param.type_oid = scratchbird::protocol::kOidInt4;
    param.format = scratchbird::protocol::kFormatBinary;
    param.data = encodeI32Le(7);

    scratchbird::client::NetworkResultSet result;
    bool suspended = false;
    auto status = client.executeServerStatement(statement_id,
                                                {param},
                                                result,
                                                1,
                                                false,
                                                &suspended,
                                                &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    EXPECT_TRUE(suspended);
    ASSERT_EQ(result.rows.size(), 1u);

    status = client.closeServerStatement(statement_id, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::OK) << ctx.message;

    status = client.closeServerStatement(statement_id, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::INVALID_ARGUMENT);
    client.disconnect();

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, ExecuteSblrAndAttachFlowsRoundTrip) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::SblrExecute,
         {{scratchbird::protocol::MessageType::RowDescription,
           buildRowDescriptionPayload({{"s", scratchbird::protocol::kOidText}})},
          {scratchbird::protocol::MessageType::DataRow,
           buildDataRowPayload({std::vector<uint8_t>{'o', 'k'}})},
          {scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 1, 0, "SELECT 1")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 40, 40)}},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             uint64_t hash = 0;
             uint16_t param_count = 0;
             std::vector<uint32_t> lengths;
             if (!parseSblrPayload(msg.body, hash, param_count, lengths, error)) {
                 return false;
             }
             if (hash != 0xAABBCCDDEEFF0011ULL) {
                 error = "unexpected sblr hash";
                 return false;
             }
             if (param_count != 1 || lengths.size() != 1 || lengths[0] == 0xFFFFFFFFu) {
                 error = "unexpected sblr parameters";
                 return false;
             }
             return true;
         }},
        {scratchbird::protocol::MessageType::AttachCreate,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 0, 0, "ATTACH CREATE")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 41, 41)}}},
        {scratchbird::protocol::MessageType::AttachDetach,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 0, 0, "ATTACH DETACH")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 42, 42)}}},
        {scratchbird::protocol::MessageType::AttachList, {}},
        {scratchbird::protocol::MessageType::Sync,
         {{scratchbird::protocol::MessageType::RowDescription,
           buildRowDescriptionPayload(
               {{"db_name", scratchbird::protocol::kOidText},
                {"mode", scratchbird::protocol::kOidText}})},
          {scratchbird::protocol::MessageType::DataRow,
           buildDataRowPayload(
               {std::vector<uint8_t>{'m', 'a', 'i', 'n'},
                std::vector<uint8_t>{'r', 'e', 'a', 'd', '_', 'w', 'r', 'i', 't', 'e'}})},
          {scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 1, 0, "ATTACH LIST")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 43, 43)}}}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::NetworkClient client;
    auto cfg = makeLoopbackConfig(harness.port);
    scratchbird::core::ErrorContext ctx;
    ASSERT_EQ(client.connect(cfg, &ctx), scratchbird::core::Status::OK) << ctx.message;

    scratchbird::protocol::ParamValue param;
    param.type_oid = scratchbird::protocol::kOidInt4;
    param.format = scratchbird::protocol::kFormatBinary;
    param.data = encodeI32Le(5);

    scratchbird::client::NetworkResultSet sblr_result;
    auto status = client.executeSblr(0xAABBCCDDEEFF0011ULL,
                                     std::vector<uint8_t>{0x10, 0x20, 0x30},
                                     {param},
                                     sblr_result,
                                     &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    ASSERT_EQ(sblr_result.rows.size(), 1u);

    EXPECT_EQ(client.attachCreate("rw", "main", &ctx), scratchbird::core::Status::OK) << ctx.message;
    EXPECT_EQ(client.attachDetach(&ctx), scratchbird::core::Status::OK) << ctx.message;

    scratchbird::client::NetworkResultSet attach_rows;
    status = client.attachList(attach_rows, &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    ASSERT_EQ(attach_rows.rows.size(), 1u);
    ASSERT_EQ(attach_rows.columns.size(), 2u);
    EXPECT_EQ(attach_rows.columns[0].name, "db_name");
    client.disconnect();

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, MetadataQueryReturnsColumnMetadataAndTypedValues) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::Query,
         {{scratchbird::protocol::MessageType::RowDescription,
           buildRowDescriptionPayload(
               {{"table_id", scratchbird::protocol::kOidInt8},
                {"table_name", scratchbird::protocol::kOidText}})},
          {scratchbird::protocol::MessageType::DataRow,
           buildDataRowPayload(
               {encodeI64Le(42),
                std::vector<uint8_t>{'c', 'u', 's', 't', 'o', 'm', 'e', 'r', 's'}})},
          {scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 1, 0, "SELECT 1")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 50, 50)}},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string sql;
             if (!parseQueryPayloadSql(msg.body, sql, error)) {
                 return false;
             }
             if (sql != sb_metadata_tables_query()) {
                 error = "unexpected metadata SQL";
                 return false;
             }
             return true;
         }}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    const std::string conn_str = "scratchbird://127.0.0.1:" + std::to_string(harness.port) +
                                 "/main?sslmode=disable&autocommit=false";
    sb_error err{};
    sb_connection* conn = sb_connect(conn_str.c_str(), &err);
    ASSERT_NE(conn, nullptr) << err.message;

    sb_result* result = sb_metadata_query(conn, "tables", &err);
    ASSERT_NE(result, nullptr) << err.message;
    ASSERT_EQ(sb_column_count(result), 2);

    sb_column_meta id_col{};
    ASSERT_EQ(sb_get_column_meta(result, 0, &id_col), SB_OK);
    EXPECT_STREQ(id_col.name, "table_id");
    EXPECT_EQ(id_col.type, SB_TYPE_BIGINT);

    sb_column_meta name_col{};
    ASSERT_EQ(sb_get_column_meta(result, 1, &name_col), SB_OK);
    EXPECT_STREQ(name_col.name, "table_name");
    EXPECT_EQ(name_col.type, SB_TYPE_TEXT);

    sb_row row{};
    ASSERT_EQ(sb_fetch(result, &row, &err), SB_OK);
    sb_value id_val{};
    ASSERT_EQ(sb_value_get(&row, 0, &id_val), SB_OK);
    EXPECT_EQ(id_val.data.bigint_val, 42);
    size_t name_len = 0;
    const char* name_ptr = sb_get_string(&row, 1, &name_len);
    ASSERT_NE(name_ptr, nullptr);
    EXPECT_EQ(std::string(name_ptr, name_len), "customers");

    sb_result_free(result);
    sb_disconnect(conn);

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, CApiSavepointAndSqlStateMappingAtBoundary) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::TxnSavepoint,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 0, 0, "SAVEPOINT")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 77, 61)}}},
        {scratchbird::protocol::MessageType::TxnRelease,
         {{scratchbird::protocol::MessageType::Error,
           buildErrorPayload("ERROR", "25006", "read only transaction")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 77, 62)}}}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    const std::string conn_str = "scratchbird://127.0.0.1:" + std::to_string(harness.port) +
                                 "/main?sslmode=disable&autocommit=false";
    sb_error err{};
    sb_connection* conn = sb_connect(conn_str.c_str(), &err);
    ASSERT_NE(conn, nullptr) << err.message;

    EXPECT_EQ(sb_tx_begin(conn, &err), SB_OK) << err.message;
    EXPECT_EQ(sb_tx_savepoint(conn, "sp_c", &err), SB_OK) << err.message;
    EXPECT_EQ(sb_tx_release_savepoint(conn, "sp_c", &err), SB_ERR_TXN_CONFLICT);
    EXPECT_EQ(err.code, SB_ERR_TXN_CONFLICT);

    sb_disconnect(conn);
    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, FeatureNotSupportedMapsToCNotImplemented) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::Query,
         {{scratchbird::protocol::MessageType::Error,
           buildErrorPayload("ERROR", "0A000", "feature not supported")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 63, 63)}}}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    const std::string conn_str = "scratchbird://127.0.0.1:" + std::to_string(harness.port) +
                                 "/main?sslmode=disable&autocommit=false";
    sb_error err{};
    sb_connection* conn = sb_connect(conn_str.c_str(), &err);
    ASSERT_NE(conn, nullptr) << err.message;

    sb_result* result = sb_execute(conn, "SELECT unsupported_feature()", &err);
    EXPECT_EQ(result, nullptr);
    EXPECT_EQ(err.code, SB_ERR_NOT_IMPLEMENTED);

    sb_disconnect(conn);
    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, CApiPreparedAndDormantCapabilitiesStayExplicit) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    EXPECT_EQ(sb_supports_prepared_transactions(), 1);
    EXPECT_EQ(sb_supports_dormant_reattach(), 0);
    EXPECT_EQ(sb_supports_portal_resume(), 0);

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::Query,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 0, 0, "PREPARE TRANSACTION")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 64, 64)}},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string sql;
             if (!parseQueryPayloadSql(msg.body, sql, error)) {
                 return false;
             }
             if (sql != "PREPARE TRANSACTION 'gid''one'") {
                 error = "unexpected prepare transaction SQL";
                 return false;
             }
             return true;
         }},
        {scratchbird::protocol::MessageType::Query,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 0, 0, "COMMIT PREPARED")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 65, 65)}},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string sql;
             if (!parseQueryPayloadSql(msg.body, sql, error)) {
                 return false;
             }
             if (sql != "COMMIT PREPARED 'gid''one'") {
                 error = "unexpected commit prepared SQL";
                 return false;
             }
             return true;
         }},
        {scratchbird::protocol::MessageType::Query,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 0, 0, "ROLLBACK PREPARED")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 66, 66)}},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string sql;
             if (!parseQueryPayloadSql(msg.body, sql, error)) {
                 return false;
             }
             if (sql != "ROLLBACK PREPARED 'gid''one'") {
                 error = "unexpected rollback prepared SQL";
                 return false;
             }
             return true;
         }}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    const std::string conn_str = "scratchbird://127.0.0.1:" + std::to_string(harness.port) +
                                 "/main?sslmode=disable&autocommit=false";
    sb_error err{};
    sb_connection* conn = sb_connect(conn_str.c_str(), &err);
    ASSERT_NE(conn, nullptr) << err.message;

    EXPECT_EQ(sb_tx_prepare_transaction(conn, " gid'one ", &err), SB_OK) << err.message;
    EXPECT_EQ(sb_tx_commit_prepared(conn, " gid'one ", &err), SB_OK) << err.message;
    EXPECT_EQ(sb_tx_rollback_prepared(conn, " gid'one ", &err), SB_OK) << err.message;

    EXPECT_EQ(sb_tx_detach_to_dormant(conn, &err), SB_ERR_NOT_IMPLEMENTED);
    EXPECT_EQ(err.code, SB_ERR_NOT_IMPLEMENTED);
    EXPECT_NE(std::string(err.message).find("dormant"), std::string::npos);

    EXPECT_EQ(sb_tx_reattach_dormant(conn, "dormant-1", "token", &err), SB_ERR_NOT_IMPLEMENTED);
    EXPECT_EQ(err.code, SB_ERR_NOT_IMPLEMENTED);

    EXPECT_EQ(sb_tx_prepare_transaction(conn, "   ", &err), SB_ERR_SYNTAX);
    EXPECT_EQ(err.code, SB_ERR_SYNTAX);

    sb_disconnect(conn);
    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, CppPreparedDormantAndCapabilitySurfacesStayExplicit) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    EXPECT_TRUE(scratchbird::client::Connection::supportsPreparedTransactions());
    EXPECT_FALSE(scratchbird::client::Connection::supportsDormantReattach());
    EXPECT_FALSE(scratchbird::client::Connection::supportsPortalResume());

    std::string sql;
    scratchbird::core::ErrorContext builder_ctx;
    auto status = scratchbird::client::Connection::buildPreparedTransactionSql(
        "PREPARE TRANSACTION", " cpp'gid ", &sql, &builder_ctx);
    ASSERT_EQ(status, scratchbird::core::Status::OK);
    EXPECT_EQ(sql, "PREPARE TRANSACTION 'cpp''gid'");

    scratchbird::core::ErrorContext blank_ctx;
    sql.clear();
    status = scratchbird::client::Connection::buildPreparedTransactionSql(
        "PREPARE TRANSACTION", "   ", &sql, &blank_ctx);
    EXPECT_EQ(status, scratchbird::core::Status::SYNTAX_ERROR);
    EXPECT_STREQ(blank_ctx.sqlstate, scratchbird::core::SQLSTATE_SYNTAX_ERROR);

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::Query,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 0, 0, "PREPARE TRANSACTION")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 67, 67)}},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string sql_text;
             if (!parseQueryPayloadSql(msg.body, sql_text, error)) {
                 return false;
             }
             if (sql_text != "PREPARE TRANSACTION 'cpp''gid'") {
                 error = "unexpected C++ prepare transaction SQL";
                 return false;
             }
             return true;
         }},
        {scratchbird::protocol::MessageType::Query,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 0, 0, "COMMIT PREPARED")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 68, 68)}},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string sql_text;
             if (!parseQueryPayloadSql(msg.body, sql_text, error)) {
                 return false;
             }
             if (sql_text != "COMMIT PREPARED 'cpp''gid'") {
                 error = "unexpected C++ commit prepared SQL";
                 return false;
             }
             return true;
         }},
        {scratchbird::protocol::MessageType::Query,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 0, 0, "ROLLBACK PREPARED")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 69, 69)}},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string sql_text;
             if (!parseQueryPayloadSql(msg.body, sql_text, error)) {
                 return false;
             }
             if (sql_text != "ROLLBACK PREPARED 'cpp''gid'") {
                 error = "unexpected C++ rollback prepared SQL";
                 return false;
             }
             return true;
         }}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    scratchbird::client::ConnectionConfig config;
    config.database_name = "main";
    config.host = "127.0.0.1";
    config.tcp_port = harness.port;
    config.ssl_mode = "disable";
    config.auto_commit = false;

    scratchbird::client::Connection conn;
    scratchbird::core::ErrorContext connect_ctx;
    status = conn.connect(config, &connect_ctx);
    ASSERT_EQ(status, scratchbird::core::Status::OK) << connect_ctx.message;

    scratchbird::core::ErrorContext txn_ctx;
    status = conn.prepareTransaction(" cpp'gid ", &txn_ctx);
    EXPECT_EQ(status, scratchbird::core::Status::OK) << txn_ctx.message;
    status = conn.commitPrepared(" cpp'gid ", &txn_ctx);
    EXPECT_EQ(status, scratchbird::core::Status::OK) << txn_ctx.message;
    status = conn.rollbackPrepared(" cpp'gid ", &txn_ctx);
    EXPECT_EQ(status, scratchbird::core::Status::OK) << txn_ctx.message;

    scratchbird::core::ErrorContext dormant_ctx;
    status = conn.detachToDormant(&dormant_ctx);
    EXPECT_EQ(status, scratchbird::core::Status::NOT_IMPLEMENTED);
    EXPECT_STREQ(dormant_ctx.sqlstate, scratchbird::core::SQLSTATE_FEATURE_NOT_SUPPORTED);

    scratchbird::core::ErrorContext reattach_ctx;
    status = conn.reattachDormant("dormant-1", "token", &reattach_ctx);
    EXPECT_EQ(status, scratchbird::core::Status::NOT_IMPLEMENTED);
    EXPECT_STREQ(reattach_ctx.sqlstate, scratchbird::core::SQLSTATE_FEATURE_NOT_SUPPORTED);

    conn.disconnect();
    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, ArrayBindUsesDefaultOutboundOidMapping) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::Parse, {}},
        {scratchbird::protocol::MessageType::Describe, {}},
        {scratchbird::protocol::MessageType::Sync,
         {{scratchbird::protocol::MessageType::ParameterDescription,
           buildParameterDescriptionPayload({scratchbird::protocol::kOidTextArray})},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 70, 70)}}},
        {scratchbird::protocol::MessageType::Bind,
         {},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             if (msg.body.size() < 18) {
                 error = "bind payload truncated";
                 return false;
             }
             // Skip portal and statement name.
             size_t offset = 0;
             if (offset + 4 > msg.body.size()) {
                 error = "bind payload truncated";
                 return false;
             }
             const uint32_t portal_len = readU32Le(msg.body.data() + offset);
             offset += 4 + portal_len;
             if (offset + 4 > msg.body.size()) {
                 error = "bind payload statement truncated";
                 return false;
             }
             const uint32_t stmt_len = readU32Le(msg.body.data() + offset);
             offset += 4 + stmt_len;
             if (offset + 2 > msg.body.size()) {
                 error = "bind payload format count truncated";
                 return false;
             }
             const uint16_t fmt_count = readU16Le(msg.body.data() + offset);
             offset += 2 + static_cast<size_t>(fmt_count) * 2;
             if (offset + 4 > msg.body.size()) {
                 error = "bind payload parameter count truncated";
                 return false;
             }
             const uint16_t param_count = readU16Le(msg.body.data() + offset);
             offset += 4;
             if (param_count != 1 || offset + 4 > msg.body.size()) {
                 error = "bind payload parameter vector mismatch";
                 return false;
             }
             const uint32_t param_len = readU32Le(msg.body.data() + offset);
             if (param_len == 0 || param_len == 0xFFFFFFFFu) {
                 error = "expected non-null array parameter payload";
                 return false;
             }
             return true;
         }},
        {scratchbird::protocol::MessageType::Execute,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 1, 0, "SELECT 1")}}},
        {scratchbird::protocol::MessageType::Sync,
         {{scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 71, 71)}}}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    const std::string conn_str = "scratchbird://127.0.0.1:" + std::to_string(harness.port) +
                                 "/main?sslmode=disable&autocommit=false";
    sb_error err{};
    sb_connection* conn = sb_connect(conn_str.c_str(), &err);
    ASSERT_NE(conn, nullptr) << err.message;

    sb_prepared* stmt = sb_prepare(conn, "SELECT $1", &err);
    ASSERT_NE(stmt, nullptr) << err.message;

    const std::string array_text = "{1,2,3}";
    sb_value v{};
    v.type = SB_TYPE_ARRAY;
    v.is_null = 0;
    v.data.binary_val.data = reinterpret_cast<const uint8_t*>(array_text.data());
    v.data.binary_val.length = array_text.size();
    EXPECT_EQ(sb_bind_index(stmt, 1, &v, &err), SB_OK) << err.message;

    sb_result* r = sb_execute_prepared(stmt, &err);
    ASSERT_NE(r, nullptr) << err.message;

    sb_result_free(r);
    sb_prepared_free(stmt);
    sb_disconnect(conn);
    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, StatementCacheAndLeakDetectorLifecycle) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::Parse, {}},
        {scratchbird::protocol::MessageType::Describe, {}},
        {scratchbird::protocol::MessageType::Sync,
         {{scratchbird::protocol::MessageType::ParameterDescription,
           buildParameterDescriptionPayload({})},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 80, 80)}}},
        {scratchbird::protocol::MessageType::Parse, {}},
        {scratchbird::protocol::MessageType::Describe, {}},
        {scratchbird::protocol::MessageType::Sync,
         {{scratchbird::protocol::MessageType::ParameterDescription,
           buildParameterDescriptionPayload({})},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 81, 81)}}},
        {scratchbird::protocol::MessageType::Parse, {}},
        {scratchbird::protocol::MessageType::Describe, {}},
        {scratchbird::protocol::MessageType::Sync,
         {{scratchbird::protocol::MessageType::ParameterDescription,
           buildParameterDescriptionPayload({})},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 82, 82)}}}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    const std::string conn_str = "scratchbird://127.0.0.1:" + std::to_string(harness.port) +
                                 "/main?sslmode=disable&autocommit=false";
    sb_error err{};
    sb_connection* conn = sb_connect(conn_str.c_str(), &err);
    ASSERT_NE(conn, nullptr) << err.message;

    sb_statement_cache* cache = sb_stmt_cache_create(1);
    ASSERT_NE(cache, nullptr);

    sb_prepared* stmt1 = sb_stmt_cache_get(cache, conn, "SELECT 1", &err);
    ASSERT_NE(stmt1, nullptr) << err.message;
    sb_prepared* stmt1_cached = sb_stmt_cache_get(cache, conn, "SELECT 1", &err);
    ASSERT_EQ(stmt1, stmt1_cached);

    sb_prepared* stmt2 = sb_stmt_cache_get(cache, conn, "SELECT 2", &err);
    ASSERT_NE(stmt2, nullptr) << err.message;
    ASSERT_NE(stmt1, stmt2);

    sb_prepared* stmt1_reprepared = sb_stmt_cache_get(cache, conn, "SELECT 1", &err);
    ASSERT_NE(stmt1_reprepared, nullptr) << err.message;
    ASSERT_NE(stmt1_reprepared, stmt2);

    sb_stmt_cache_destroy(cache);

    scratchbird::sb_leak_detection_config leak_cfg =
        scratchbird::sb_leak_detection_config_default();
    leak_cfg.threshold_ms = 1000;
    leak_cfg.check_interval_ms = 50;
    scratchbird::sb_leak_detector* detector = scratchbird::sb_leak_detector_create(&leak_cfg);
    ASSERT_NE(detector, nullptr);
    scratchbird::sb_leak_detector_start(detector);
    scratchbird::sb_leak_detector_checkout(detector, "conn-test");
    EXPECT_EQ(scratchbird::sb_leak_detector_get_active_count(detector), 1u);
    scratchbird::sb_leak_detector_checkin(detector, "conn-test");
    EXPECT_EQ(scratchbird::sb_leak_detector_get_active_count(detector), 0u);
    scratchbird::sb_leak_detector_stop(detector);
    scratchbird::sb_leak_detector_destroy(detector);

    sb_disconnect(conn);
    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, PoolAcquireReleaseAndRetryUtility) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarness harness;
    setupIpv4Listener(harness);
    harness.start();

    const std::string conn_str = "scratchbird://127.0.0.1:" + std::to_string(harness.port) +
                                 "/main?sslmode=disable&autocommit=false";
    sb_pool_config cfg = sb_pool_config_default();
    cfg.min_connections = 1;
    cfg.max_connections = 2;
    cfg.acquire_timeout_seconds = 1;
    cfg.test_on_checkout = 0;

    sb_error err{};
    sb_connection_pool* pool = sb_pool_create(conn_str.c_str(), &cfg, &err);
    ASSERT_NE(pool, nullptr) << err.message;

    sb_pool_stats stats = sb_pool_get_stats(pool);
    EXPECT_EQ(stats.total_connections, 1u);
    EXPECT_EQ(stats.available_connections, 1u);

    sb_connection* conn = sb_pool_acquire(pool, &err);
    ASSERT_NE(conn, nullptr) << err.message;
    stats = sb_pool_get_stats(pool);
    EXPECT_EQ(stats.available_connections, 0u);

    sb_pool_release(pool, conn);
    stats = sb_pool_get_stats(pool);
    EXPECT_EQ(stats.available_connections, 1u);

    EXPECT_GE(sb_connection_get_age_seconds(conn), 0u);
    EXPECT_GE(sb_connection_get_idle_seconds(conn), 0u);

    struct RetryState {
        int attempts{0};
    } state;
    auto retryable = [](void* user_data) -> sb_error {
        auto* s = static_cast<RetryState*>(user_data);
        sb_error out{};
        ++s->attempts;
        if (s->attempts < 3) {
            out.code = SB_ERR_CONNECTION_FAILED;
            std::snprintf(out.message, sizeof(out.message), "retry");
            return out;
        }
        out.code = SB_OK;
        out.message[0] = '\0';
        return out;
    };
    sb_retry_config retry_cfg = sb_retry_config_default();
    retry_cfg.max_retries = 4;
    retry_cfg.base_delay_ms = 1;
    retry_cfg.max_delay_ms = 5;
    sb_error retry_result = sb_with_retry(&retry_cfg, retryable, &state);
    EXPECT_EQ(retry_result.code, SB_OK);
    EXPECT_EQ(state.attempts, 3);

    struct GovernanceRetryState {
        int attempts{0};
    } governed_state;
    auto governed = [](void* user_data) -> sb_error {
        auto* s = static_cast<GovernanceRetryState*>(user_data);
        sb_error out{};
        ++s->attempts;
        out.code = SB_ERR_TXN_ABORTED;
        std::snprintf(out.message, sizeof(out.message), "governed rollback");
        return out;
    };
    governed_state.attempts = 0;
    sb_error governed_result = sb_with_retry(&retry_cfg, governed, &governed_state);
    EXPECT_EQ(governed_result.code, SB_ERR_TXN_ABORTED);
    EXPECT_EQ(governed_state.attempts, 1);

    sb_pool_destroy(pool);
    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, BatchExecuteSupportsParameterizedOperations) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::Query,
         {{scratchbird::protocol::MessageType::RowDescription,
           buildRowDescriptionPayload({{"v", scratchbird::protocol::kOidInt4}})},
          {scratchbird::protocol::MessageType::DataRow, buildDataRowPayload({encodeI32Le(1)})},
          {scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 1, 0, "SELECT 1")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 90, 90)}},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string sql;
             if (!parseQueryPayloadSql(msg.body, sql, error)) {
                 return false;
             }
             if (sql != "SELECT 1") {
                 error = "unexpected batch query SQL";
                 return false;
             }
             return true;
         }},
        {scratchbird::protocol::MessageType::Parse,
         {},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string stmt;
             std::string sql;
             if (!parseParsePayloadStatementAndSql(msg.body, stmt, sql, error)) {
                 return false;
             }
             if (sql != "SELECT $1") {
                 error = "unexpected batch parse SQL";
                 return false;
             }
             return true;
         }},
        {scratchbird::protocol::MessageType::Describe, {}},
        {scratchbird::protocol::MessageType::Sync,
         {{scratchbird::protocol::MessageType::ParameterDescription,
           buildParameterDescriptionPayload({scratchbird::protocol::kOidInt4})},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 91, 91)}}},
        {scratchbird::protocol::MessageType::Bind, {}},
        {scratchbird::protocol::MessageType::Execute,
         {{scratchbird::protocol::MessageType::RowDescription,
           buildRowDescriptionPayload({{"v", scratchbird::protocol::kOidInt4}})},
          {scratchbird::protocol::MessageType::DataRow, buildDataRowPayload({encodeI32Le(7)})},
          {scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 1, 0, "SELECT 1")}}},
        {scratchbird::protocol::MessageType::Sync,
         {{scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 92, 92)}}}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    const std::string conn_str = "scratchbird://127.0.0.1:" + std::to_string(harness.port) +
                                 "/main?sslmode=disable&autocommit=false";
    sb_error err{};
    sb_connection* conn = sb_connect(conn_str.c_str(), &err);
    ASSERT_NE(conn, nullptr) << err.message;

    sb_value value{};
    value.type = SB_TYPE_INTEGER;
    value.is_null = 0;
    value.data.integer_val = 7;
    sb_value* params_row[] = {&value};

    sb_batch_operation ops[2]{};
    ops[0].sql = "SELECT 1";
    ops[0].params = nullptr;
    ops[0].param_count = 0;
    ops[1].sql = "SELECT $1";
    ops[1].params = params_row;
    ops[1].param_count = 1;

    int64_t total_rows = 0;
    int64_t* rows = sb_batch_execute(conn, ops, 2, &total_rows, &err);
    ASSERT_NE(rows, nullptr) << err.message;
    EXPECT_EQ(total_rows, 2);
    EXPECT_EQ(rows[0], 1);
    EXPECT_EQ(rows[1], 1);
    std::free(rows);

    sb_disconnect(conn);
    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, BulkInsertExecutesBinaryCopyRowset) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::Query,
         {{scratchbird::protocol::MessageType::CopyInResponse,
           scratchbird::protocol::buildCopyInResponsePayload(scratchbird::protocol::kFormatBinary,
                                                              1024)}},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string sql;
             if (!parseQueryPayloadSql(msg.body, sql, error)) {
                 return false;
             }
             if (sql != "COPY \"main\".\"users\" (\"id\", \"name\") FROM STDIN") {
                 error = "unexpected bulk COPY SQL";
                 return false;
             }
             return true;
         }},
        {scratchbird::protocol::MessageType::CopyData,
         {},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             if (msg.body.size() < 12 ||
                 msg.body[0] != 'S' || msg.body[1] != 'B' ||
                 msg.body[2] != 'C' || msg.body[3] != 'P' ||
                 msg.body[4] != 1) {
                 error = "binary COPY frame header mismatch";
                 return false;
             }
             size_t offset = 8;
             const uint32_t row_count = readU32Le(msg.body.data() + offset);
             offset += 4;
             if (row_count != 2) {
                 error = "binary COPY row count mismatch";
                 return false;
             }
             std::vector<std::vector<std::pair<std::string, std::string>>> rows;
             for (uint32_t row_index = 0; row_index < row_count; ++row_index) {
                 if (offset + 4 > msg.body.size()) {
                     error = "binary COPY field count truncated";
                     return false;
                 }
                 const uint32_t field_count = readU32Le(msg.body.data() + offset);
                 offset += 4;
                 std::vector<std::pair<std::string, std::string>> fields;
                 for (uint32_t field_index = 0; field_index < field_count; ++field_index) {
                     if (offset + 4 > msg.body.size()) {
                         error = "binary COPY field name length truncated";
                         return false;
                     }
                     const uint32_t name_size = readU32Le(msg.body.data() + offset);
                     offset += 4;
                     if (offset + name_size + 1 + 4 > msg.body.size()) {
                         error = "binary COPY field header truncated";
                         return false;
                     }
                     const std::string name(
                         reinterpret_cast<const char*>(msg.body.data() + offset), name_size);
                     offset += name_size;
                     const bool is_null = msg.body[offset++] != 0;
                     const uint32_t value_size = readU32Le(msg.body.data() + offset);
                     offset += 4;
                     if (offset + value_size > msg.body.size()) {
                         error = "binary COPY field value truncated";
                         return false;
                     }
                     std::string value;
                     if (!is_null) {
                         value.assign(reinterpret_cast<const char*>(msg.body.data() + offset),
                                      value_size);
                     }
                     offset += value_size;
                     fields.push_back({name, value});
                 }
                 rows.push_back(std::move(fields));
             }
             if (offset != msg.body.size()) {
                 error = "binary COPY frame trailing bytes";
                 return false;
             }
             const std::vector<std::vector<std::pair<std::string, std::string>>> expected = {
                 {{"id", "1"}, {"name", "alice"}},
                 {{"id", "2"}, {"name", "bob"}}
             };
             if (rows != expected) {
                 error = "binary COPY fields mismatch";
                 return false;
             }
             return true;
         }},
        {scratchbird::protocol::MessageType::CopyDone,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 2, 0, "COPY 2")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 95, 95)}}}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    const std::string conn_str = "scratchbird://127.0.0.1:" + std::to_string(harness.port) +
                                 "/main?sslmode=disable&autocommit=false";
    sb_error err{};
    sb_connection* conn = sb_connect(conn_str.c_str(), &err);
    ASSERT_NE(conn, nullptr) << err.message;

    const char* columns[] = {"id", "name"};
    sb_value row1[2]{};
    row1[0].type = SB_TYPE_INTEGER;
    row1[0].is_null = 0;
    row1[0].data.integer_val = 1;
    row1[1].type = SB_TYPE_TEXT;
    row1[1].is_null = 0;
    row1[1].data.string_val.data = "alice";
    row1[1].data.string_val.length = 5;

    sb_value row2[2]{};
    row2[0].type = SB_TYPE_INTEGER;
    row2[0].is_null = 0;
    row2[0].data.integer_val = 2;
    row2[1].type = SB_TYPE_TEXT;
    row2[1].is_null = 0;
    row2[1].data.string_val.data = "bob";
    row2[1].data.string_val.length = 3;

    const sb_value* rows[] = {row1, row2};
    int64_t rows_inserted = 0;
    const int rc = sb_bulk_insert(
        conn,
        "main.users",
        columns,
        2,
        rows,
        2,
        &rows_inserted,
        &err);
    EXPECT_EQ(rc, SB_OK) << err.message;
    EXPECT_EQ(rows_inserted, 2);

    sb_disconnect(conn);
    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverTxnExecParityTest, CApiMetadataSchemaPayloadIncludesDdlEditorFields) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::Query,
         {{scratchbird::protocol::MessageType::RowDescription,
           buildRowDescriptionPayload({{"schema_name", scratchbird::protocol::kOidText}})},
          {scratchbird::protocol::MessageType::DataRow,
           buildDataRowPayload({std::vector<uint8_t>{'u','s','e','r','s','.','a','l','i','c','e','.','d','e','v'}})},
          {scratchbird::protocol::MessageType::DataRow,
           buildDataRowPayload({std::vector<uint8_t>{'u','s','e','r','s','.','b','o','b','.','d','e','v'}})},
          {scratchbird::protocol::MessageType::DataRow,
           buildDataRowPayload({std::vector<uint8_t>{'s','y','s'}})},
          {scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 3, 0, "SELECT 3")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 96, 96)}},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string sql;
             if (!parseQueryPayloadSql(msg.body, sql, error)) {
                 return false;
             }
             if (sql != sb_metadata_schemas_query()) {
                 error = "unexpected metadata schemas SQL";
                 return false;
             }
             return true;
         }}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    const std::string conn_str = "scratchbird://127.0.0.1:" + std::to_string(harness.port) +
                                 "/main?sslmode=disable&autocommit=false";
    sb_error err{};
    sb_connection* conn = sb_connect(conn_str.c_str(), &err);
    ASSERT_NE(conn, nullptr) << err.message;

    char* payload_raw = sb_metadata_schema_payload(conn, "users.%", 1, &err);
    ASSERT_NE(payload_raw, nullptr) << err.message;

    const auto payload = nlohmann::json::parse(payload_raw);
    EXPECT_EQ(payload["schemaPattern"], "users.%");
    EXPECT_TRUE(payload["expandSchemaParents"].get<bool>());
    EXPECT_EQ(
        payload["schemaPaths"].get<std::vector<std::string>>(),
        (std::vector<std::string>{"users", "users.alice", "users.alice.dev", "users.bob", "users.bob.dev"}));
    ASSERT_TRUE(payload["schemaTree"].is_array());
    ASSERT_FALSE(payload["schemaTree"].empty());
    EXPECT_EQ(payload["schemaTree"][0]["name"], "users");
    EXPECT_EQ(payload["schemaTree"][0]["path"], "users");
    sb_memory_free(payload_raw);

    sb_disconnect(conn);
    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverCppApiClosureTest, ConnectionTracksReadyTransactionStatusLifecycle) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::TxnCommit,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 0, 0, "COMMIT")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 2, 2)}}},
        {scratchbird::protocol::MessageType::TxnRollback,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 0, 0, "ROLLBACK")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 4, 4)}}}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    const std::string conn_str = "scratchbird://127.0.0.1:" + std::to_string(harness.port) +
                                 "/main?sslmode=disable&autocommit=false";

    scratchbird::client::Connection conn;
    scratchbird::core::ErrorContext ctx;
    ASSERT_EQ(conn.connect(conn_str, "", "", &ctx), scratchbird::core::Status::OK) << ctx.message;
    EXPECT_TRUE(conn.isConnected());
    EXPECT_TRUE(conn.inTransaction());
    EXPECT_EQ(conn.getState(), scratchbird::client::ConnectionState::IN_TRANSACTION);

    ASSERT_EQ(conn.beginTransaction(&ctx), scratchbird::core::Status::OK) << ctx.message;
    EXPECT_TRUE(conn.inTransaction());
    EXPECT_EQ(conn.getState(), scratchbird::client::ConnectionState::IN_TRANSACTION);

    ASSERT_EQ(conn.commit(&ctx), scratchbird::core::Status::OK) << ctx.message;
    EXPECT_TRUE(conn.inTransaction());
    EXPECT_EQ(conn.getState(), scratchbird::client::ConnectionState::IN_TRANSACTION);

    ASSERT_EQ(conn.beginTransaction(&ctx), scratchbird::core::Status::OK) << ctx.message;
    EXPECT_TRUE(conn.inTransaction());
    EXPECT_EQ(conn.getState(), scratchbird::client::ConnectionState::IN_TRANSACTION);

    ASSERT_EQ(conn.rollback(&ctx), scratchbird::core::Status::OK) << ctx.message;
    EXPECT_TRUE(conn.inTransaction());
    EXPECT_EQ(conn.getState(), scratchbird::client::ConnectionState::IN_TRANSACTION);

    conn.disconnect();
    EXPECT_FALSE(conn.isConnected());
    EXPECT_FALSE(conn.inTransaction());
    EXPECT_EQ(conn.getState(), scratchbird::client::ConnectionState::DISCONNECTED);

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverCppApiClosureTest, MetadataHelpersExecuteStableFilteredQueries) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    const std::string schema_pattern = "users.%";
    const std::string table_pattern = "orders%";

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::Query,
         {{scratchbird::protocol::MessageType::RowDescription,
           buildRowDescriptionPayload(
               {{"schema_id", scratchbird::protocol::kOidInt8},
                {"schema_name", scratchbird::protocol::kOidText},
                {"owner_id", scratchbird::protocol::kOidInt8},
                {"default_tablespace_id", scratchbird::protocol::kOidInt8}})},
          {scratchbird::protocol::MessageType::DataRow,
           buildDataRowPayload(
               {encodeI64Le(7),
                std::vector<uint8_t>{'u','s','e','r','s','.','a','l','i','c','e'},
                encodeI64Le(5),
                encodeI64Le(9)})},
          {scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 1, 0, "SELECT 1")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 10, 10)}},
         [schema_pattern](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string sql;
             if (!parseQueryPayloadSql(msg.body, sql, error)) {
                 return false;
             }
             if (sql != scratchbird::client::buildMetadataSchemasQuerySql(&schema_pattern)) {
                 error = "unexpected schemas metadata SQL";
                 return false;
             }
             return true;
         }},
        {scratchbird::protocol::MessageType::Query,
         {{scratchbird::protocol::MessageType::RowDescription,
           buildRowDescriptionPayload(
               {{"table_id", scratchbird::protocol::kOidInt8},
                {"schema_id", scratchbird::protocol::kOidInt8},
                {"table_name", scratchbird::protocol::kOidText},
                {"table_type", scratchbird::protocol::kOidText},
                {"owner_id", scratchbird::protocol::kOidInt8}})},
          {scratchbird::protocol::MessageType::DataRow,
           buildDataRowPayload(
               {encodeI64Le(41),
                encodeI64Le(7),
                std::vector<uint8_t>{'o','r','d','e','r','s'},
                std::vector<uint8_t>{'B','A','S','E'},
                encodeI64Le(5)})},
          {scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 1, 0, "SELECT 1")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 11, 11)}},
         [schema_pattern, table_pattern](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string sql;
             if (!parseQueryPayloadSql(msg.body, sql, error)) {
                 return false;
             }
             if (sql != scratchbird::client::buildMetadataTablesQuerySql(&schema_pattern, &table_pattern)) {
                 error = "unexpected tables metadata SQL";
                 return false;
             }
             return true;
         }},
        {scratchbird::protocol::MessageType::Query,
         {{scratchbird::protocol::MessageType::RowDescription,
           buildRowDescriptionPayload(
               {{"column_id", scratchbird::protocol::kOidInt8},
                {"table_id", scratchbird::protocol::kOidInt8},
                {"column_name", scratchbird::protocol::kOidText},
                {"data_type_id", scratchbird::protocol::kOidInt4},
                {"data_type_name", scratchbird::protocol::kOidText},
                {"ordinal_position", scratchbird::protocol::kOidInt4},
                {"is_nullable", scratchbird::protocol::kOidBool},
                {"default_value", scratchbird::protocol::kOidText},
                {"domain_id", scratchbird::protocol::kOidInt8},
                {"collation_id", scratchbird::protocol::kOidInt8},
                {"charset_id", scratchbird::protocol::kOidInt8},
                {"is_identity", scratchbird::protocol::kOidBool},
                {"is_generated", scratchbird::protocol::kOidBool},
                {"generation_expression", scratchbird::protocol::kOidText}})},
          {scratchbird::protocol::MessageType::DataRow,
           buildDataRowPayload(
               {encodeI64Le(90),
                encodeI64Le(41),
                std::vector<uint8_t>{'i','d'},
                encodeI32Le(23),
                std::vector<uint8_t>{'i','n','t','4'},
                encodeI32Le(1),
                std::vector<uint8_t>{1},
                std::vector<uint8_t>{},
                encodeI64Le(0),
                encodeI64Le(0),
                encodeI64Le(0),
                std::vector<uint8_t>{0},
                std::vector<uint8_t>{0},
                std::vector<uint8_t>{}})},
          {scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 1, 0, "SELECT 1")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 12, 12)}},
         [schema_pattern, table_pattern](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string sql;
             if (!parseQueryPayloadSql(msg.body, sql, error)) {
                 return false;
             }
             if (sql != scratchbird::client::buildMetadataColumnsQuerySql(&schema_pattern, &table_pattern)) {
                 error = "unexpected columns metadata SQL";
                 return false;
             }
             return true;
         }},
        {scratchbird::protocol::MessageType::Query,
         {{scratchbird::protocol::MessageType::RowDescription,
           buildRowDescriptionPayload(
               {{"index_id", scratchbird::protocol::kOidInt8},
                {"table_id", scratchbird::protocol::kOidInt8},
                {"index_name", scratchbird::protocol::kOidText},
                {"index_type", scratchbird::protocol::kOidText},
                {"is_unique", scratchbird::protocol::kOidBool}})},
          {scratchbird::protocol::MessageType::DataRow,
           buildDataRowPayload(
               {encodeI64Le(301),
                encodeI64Le(41),
                std::vector<uint8_t>{'o','r','d','e','r','s','_','p','k'},
                std::vector<uint8_t>{'b','t','r','e','e'},
                std::vector<uint8_t>{1}})},
          {scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 1, 0, "SELECT 1")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 13, 13)}},
         [schema_pattern, table_pattern](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string sql;
             if (!parseQueryPayloadSql(msg.body, sql, error)) {
                 return false;
             }
             if (sql != scratchbird::client::buildMetadataIndexesQuerySql(&schema_pattern, &table_pattern)) {
                 error = "unexpected indexes metadata SQL";
                 return false;
             }
             return true;
         }}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    const std::string conn_str = "scratchbird://127.0.0.1:" + std::to_string(harness.port) +
                                 "/main?sslmode=disable&autocommit=false";

    scratchbird::client::Connection conn;
    scratchbird::core::ErrorContext ctx;
    ASSERT_EQ(conn.connect(conn_str, "", "", &ctx), scratchbird::core::Status::OK) << ctx.message;

    scratchbird::client::ResultSet schemas;
    ASSERT_EQ(conn.schemas(&schemas, schema_pattern, &ctx), scratchbird::core::Status::OK) << ctx.message;
    ASSERT_EQ(schemas.getColumnCount(), 4u);
    EXPECT_EQ(schemas.getColumnType(0), SB_TYPE_BIGINT);
    EXPECT_EQ(schemas.getColumnTypeOid(1), scratchbird::protocol::kOidText);
    EXPECT_EQ(schemas.getColumnFormat(1), scratchbird::protocol::kFormatBinary);
    EXPECT_TRUE(schemas.isColumnNullable(1));
    ASSERT_TRUE(schemas.next());
    EXPECT_EQ(schemas.getString("schema_name"), "users.alice");

    scratchbird::client::ResultSet tables;
    ASSERT_EQ(conn.tables(&tables, schema_pattern, table_pattern, &ctx), scratchbird::core::Status::OK)
        << ctx.message;
    ASSERT_TRUE(tables.next());
    EXPECT_EQ(tables.getString("table_name"), "orders");

    scratchbird::client::ResultSet columns;
    ASSERT_EQ(conn.columns(&columns, schema_pattern, table_pattern, &ctx), scratchbird::core::Status::OK)
        << ctx.message;
    ASSERT_TRUE(columns.next());
    EXPECT_EQ(columns.getString("column_name"), "id");
    EXPECT_EQ(columns.getString("data_type_name"), "int4");

    scratchbird::client::ResultSet indexes;
    ASSERT_EQ(conn.indexes(&indexes, schema_pattern, table_pattern, &ctx), scratchbird::core::Status::OK)
        << ctx.message;
    ASSERT_TRUE(indexes.next());
    EXPECT_EQ(indexes.getString("index_name"), "orders_pk");

    conn.disconnect();
    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverCppApiClosureTest, PreparedStatementWrapperExecutesRoundTrip) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::Parse, {}},
        {scratchbird::protocol::MessageType::Describe, {}},
        {scratchbird::protocol::MessageType::Sync,
         {{scratchbird::protocol::MessageType::ParameterDescription,
           buildParameterDescriptionPayload({scratchbird::protocol::kOidInt4})},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 40, 40)}}},
        {scratchbird::protocol::MessageType::Bind, {}},
        {scratchbird::protocol::MessageType::Execute,
         {{scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 1, 0, "UPDATE 1")}}},
        {scratchbird::protocol::MessageType::Sync,
         {{scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 41, 41)}}}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    const std::string conn_str = "scratchbird://127.0.0.1:" + std::to_string(harness.port) +
                                 "/main?sslmode=disable&autocommit=false";

    scratchbird::client::Connection conn;
    scratchbird::core::ErrorContext ctx;
    ASSERT_EQ(conn.connect(conn_str, "", "", &ctx), scratchbird::core::Status::OK) << ctx.message;

    scratchbird::client::PreparedStatement stmt;
    ASSERT_EQ(conn.prepare("UPDATE t SET v = $1", &stmt, &ctx), scratchbird::core::Status::OK)
        << ctx.message;
    EXPECT_TRUE(stmt.isValid());
    EXPECT_EQ(stmt.getParameterCount(), 1u);

    stmt.setInt32(1, 7);
    int64_t rows_affected = 0;
    ASSERT_EQ(stmt.execute(&rows_affected, &ctx), scratchbird::core::Status::OK) << ctx.message;
    EXPECT_EQ(rows_affected, 1);
    EXPECT_TRUE(conn.inTransaction());
    EXPECT_EQ(conn.getState(), scratchbird::client::ConnectionState::IN_TRANSACTION);

    conn.disconnect();
    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverCppApiClosureTest, ConnectionPoolLeaseAutoReleasesToPool) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarness harness;
    setupIpv4Listener(harness);
    harness.start();

    const std::string conn_str = "scratchbird://127.0.0.1:" + std::to_string(harness.port) +
                                 "/main?sslmode=disable&autocommit=false";

    sb_pool_config pool_cfg = sb_pool_config_default();
    pool_cfg.min_connections = 1;
    pool_cfg.max_connections = 2;
    pool_cfg.acquire_timeout_seconds = 1;
    pool_cfg.test_on_checkout = 0;

    scratchbird::client::ConnectionPool pool;
    sb_error err{};
    ASSERT_TRUE(pool.open(conn_str, pool_cfg, &err)) << err.message;
    EXPECT_TRUE(pool.isOpen());
    EXPECT_EQ(pool.stats().available_connections, 1u);

    {
        auto lease = pool.acquire(&err);
        ASSERT_TRUE(lease.valid()) << err.message;
        EXPECT_NE(lease.raw(), nullptr);
        EXPECT_EQ(pool.stats().available_connections, 0u);
    }

    EXPECT_EQ(pool.stats().available_connections, 1u);
    pool.close();
    EXPECT_FALSE(pool.isOpen());

    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverCppApiClosureTest, MetadataSchemaPayloadBuildsEditorJson) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::Query,
         {{scratchbird::protocol::MessageType::RowDescription,
           buildRowDescriptionPayload({{"schema_name", scratchbird::protocol::kOidText}})},
          {scratchbird::protocol::MessageType::DataRow,
           buildDataRowPayload({std::vector<uint8_t>{'u','s','e','r','s','.','a','l','i','c','e','.','d','e','v'}})},
          {scratchbird::protocol::MessageType::DataRow,
           buildDataRowPayload({std::vector<uint8_t>{'u','s','e','r','s','.','b','o','b','.','d','e','v'}})},
          {scratchbird::protocol::MessageType::DataRow,
           buildDataRowPayload({std::vector<uint8_t>{'s','y','s'}})},
          {scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 3, 0, "SELECT 3")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 20, 20)}},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string sql;
             if (!parseQueryPayloadSql(msg.body, sql, error)) {
                 return false;
             }
             if (sql != scratchbird::client::buildMetadataSchemasQuerySql(nullptr)) {
                 error = "unexpected schema payload metadata SQL";
                 return false;
             }
             return true;
         }}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    const std::string conn_str = "scratchbird://127.0.0.1:" + std::to_string(harness.port) +
                                 "/main?sslmode=disable&autocommit=false";

    scratchbird::client::Connection conn;
    scratchbird::core::ErrorContext ctx;
    ASSERT_EQ(conn.connect(conn_str, "", "", &ctx), scratchbird::core::Status::OK) << ctx.message;

    const std::string pattern = "users.%";
    std::string payload_json;
    ASSERT_EQ(
        conn.metadataSchemaPayload(&pattern, true, &payload_json, &ctx),
        scratchbird::core::Status::OK) << ctx.message;

    const auto payload = nlohmann::json::parse(payload_json);
    EXPECT_EQ(payload["schemaPattern"], "users.%");
    EXPECT_TRUE(payload["expandSchemaParents"].get<bool>());
    EXPECT_EQ(
        payload["schemaPaths"].get<std::vector<std::string>>(),
        (std::vector<std::string>{"users", "users.alice", "users.alice.dev", "users.bob", "users.bob.dev"}));

    conn.disconnect();
    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverCppApiClosureTest, CApiComplexTypeStringAccessReturnsBinaryBackedPayloads) {
    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    ServerHarnessConfig harness_cfg;
    harness_cfg.exchanges = {
        {scratchbird::protocol::MessageType::Query,
         {{scratchbird::protocol::MessageType::RowDescription,
           buildRowDescriptionPayload(
               {{"array_value", scratchbird::protocol::kOidTextArray},
                {"vector_value", scratchbird::protocol::kOidSbVector},
                {"macaddr_value", scratchbird::protocol::kOidMacaddr},
                {"range_value", scratchbird::protocol::kOidInt4Range}})},
          {scratchbird::protocol::MessageType::DataRow,
           buildDataRowPayload(
               {std::vector<uint8_t>{'{','1',',','2',',','3','}'},
                std::vector<uint8_t>{'[','1',',','2',',','3',']'},
                std::vector<uint8_t>{'a','a',':','b','b',':','c','c',':','d','d',':','e','e',':','f','f'},
                std::vector<uint8_t>{'[','1',',','9',']'}})},
          {scratchbird::protocol::MessageType::CommandComplete,
           buildCommandCompletePayload(0, 1, 0, "SELECT 1")},
          {scratchbird::protocol::MessageType::Ready, buildReadyPayload(1, 30, 30)}},
         [](const scratchbird::protocol::ProtocolMessage& msg, std::string& error) {
             std::string sql;
             return parseQueryPayloadSql(msg.body, sql, error) && sql == "SELECT complex_types()";
         }}
    };

    ServerHarness harness(std::move(harness_cfg));
    setupIpv4Listener(harness);
    harness.start();

    const std::string conn_str = "scratchbird://127.0.0.1:" + std::to_string(harness.port) +
                                 "/main?sslmode=disable&autocommit=false";
    sb_error err{};
    sb_connection* conn = sb_connect(conn_str.c_str(), &err);
    ASSERT_NE(conn, nullptr) << err.message;

    sb_result* result = sb_query(conn, "SELECT complex_types()", &err);
    ASSERT_NE(result, nullptr) << err.message;

    sb_column_meta array_meta{};
    ASSERT_EQ(sb_get_column_meta(result, 0, &array_meta), SB_OK);
    EXPECT_EQ(array_meta.type, SB_TYPE_ARRAY);

    sb_row row{};
    ASSERT_EQ(sb_fetch(result, &row, &err), SB_OK);

    size_t len = 0;
    const char* raw = sb_get_string(&row, 0, &len);
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(std::string(raw, len), "{1,2,3}");

    raw = sb_get_string(&row, 1, &len);
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(std::string(raw, len), "[1,2,3]");

    raw = sb_get_string(&row, 2, &len);
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(std::string(raw, len), "aa:bb:cc:dd:ee:ff");

    raw = sb_get_string(&row, 3, &len);
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(std::string(raw, len), "[1,9]");

    sb_result_free(result);
    sb_disconnect(conn);
    harness.stop();
    EXPECT_TRUE(harness.error.empty()) << harness.error;
}

TEST(DriverCppApiClosureTest, NetworkClientDisconnectClearsAbandonedSessionState) {
    scratchbird::client::NetworkClient client;
    client.connected_ = true;
    client.in_transaction_ = true;
    client.last_error_ = "stale error";
    client.next_sequence_ = 9;
    client.last_query_sequence_ = 7;
    client.query_progress_.rows_processed = 11;
    client.query_progress_.bytes_processed = 12;
    client.query_progress_.updated_at_micros = 13;
    client.session_id_.fill(0xAB);
    client.parameter_status_["attachment_id"] = "00112233-4455-6677-8899-aabbccddeeff";
    client.parameter_status_["current_txn_id"] = "42";
    client.notifications_.push_back({1, "chan", {'p'}, 1, 9});
    scratchbird::client::NetworkPreparedStatement stmt;
    stmt.valid_ = true;
    stmt.statement_name_ = "stmt_one";
    client.prepared_statements_.emplace(1, std::move(stmt));
    client.last_plan_ = std::make_unique<scratchbird::protocol::QueryPlan>();
    client.last_plan_->format = 1;
    client.last_sblr_ = std::make_unique<scratchbird::protocol::SblrCompiled>();
    client.last_sblr_->version = 2;

    client.disconnect();

    EXPECT_FALSE(client.connected_);
    EXPECT_FALSE(client.in_transaction_);
    EXPECT_TRUE(client.last_error_.empty());
    EXPECT_EQ(client.next_sequence_, 0u);
    EXPECT_EQ(client.last_query_sequence_, 0u);
    EXPECT_EQ(client.query_progress_.rows_processed, 0u);
    EXPECT_EQ(client.query_progress_.bytes_processed, 0u);
    EXPECT_EQ(client.query_progress_.updated_at_micros, 0u);
    EXPECT_TRUE(std::all_of(
        client.session_id_.begin(),
        client.session_id_.end(),
        [](uint8_t byte) { return byte == 0; }));
    EXPECT_TRUE(client.parameter_status_.empty());
    EXPECT_TRUE(client.notifications_.empty());
    EXPECT_TRUE(client.prepared_statements_.empty());
    EXPECT_EQ(client.last_plan_, nullptr);
    EXPECT_EQ(client.last_sblr_, nullptr);
}

TEST(DriverRecoveryIntegrationTest, CppReconnectDoesNotResurrectAbandonedTransaction) {
    const auto dsn = integrationDsn();
    if (!dsn.has_value()) {
        GTEST_SKIP() << "SCRATCHBIRD_TEST_DSN not set";
    }

    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    scratchbird::client::Connection conn;
    scratchbird::core::ErrorContext connect_ctx;
    ASSERT_EQ(conn.connect(*dsn, "", "", &connect_ctx), scratchbird::core::Status::OK)
        << connect_ctx.message;
    ASSERT_TRUE(conn.isConnected());
    EXPECT_TRUE(conn.inTransaction());

    scratchbird::client::ResultSet result;
    scratchbird::core::ErrorContext query_ctx;
    ASSERT_EQ(conn.executeQuery("SELECT 1", &result, &query_ctx), scratchbird::core::Status::OK)
        << query_ctx.message;
    ASSERT_TRUE(result.next());
    EXPECT_EQ(result.getInt32(0), 1);

    scratchbird::core::ErrorContext savepoint_ctx;
    ASSERT_EQ(conn.savepoint("dmrw007_live_cpp", &savepoint_ctx), scratchbird::core::Status::OK)
        << savepoint_ctx.message;

    conn.disconnect();
    ASSERT_FALSE(conn.isConnected());
    EXPECT_FALSE(conn.inTransaction());
    EXPECT_EQ(conn.getState(), scratchbird::client::ConnectionState::DISCONNECTED);

    scratchbird::core::ErrorContext reconnect_ctx;
    ASSERT_EQ(conn.connect(*dsn, "", "", &reconnect_ctx), scratchbird::core::Status::OK)
        << reconnect_ctx.message;
    ASSERT_TRUE(conn.isConnected());
    EXPECT_TRUE(conn.inTransaction());

    scratchbird::core::ErrorContext stale_savepoint_ctx;
    EXPECT_NE(conn.rollbackTo("dmrw007_live_cpp", &stale_savepoint_ctx), scratchbird::core::Status::OK);
    EXPECT_TRUE(std::strncmp(stale_savepoint_ctx.sqlstate, "3B", 2) == 0)
        << stale_savepoint_ctx.sqlstate;
    ASSERT_TRUE(conn.inTransaction());

    scratchbird::core::ErrorContext rollback_after_reopen_ctx;
    ASSERT_EQ(conn.rollback(&rollback_after_reopen_ctx), scratchbird::core::Status::OK)
        << rollback_after_reopen_ctx.message;
    ASSERT_TRUE(conn.inTransaction());

    scratchbird::core::ErrorContext fresh_savepoint_ctx;
    ASSERT_EQ(conn.savepoint("dmrw007_live_cpp_fresh", &fresh_savepoint_ctx), scratchbird::core::Status::OK)
        << fresh_savepoint_ctx.message;

    conn.disconnect();
}

TEST(DriverRecoveryIntegrationTest, CApiReconnectDoesNotReuseAbandonedTransactionState) {
    const auto dsn = integrationDsn();
    if (!dsn.has_value()) {
        GTEST_SKIP() << "SCRATCHBIRD_TEST_DSN not set";
    }

    scratchbird::network::NetworkInitGuard guard;
    ASSERT_TRUE(guard.isInitialized());

    sb_error err{};
    sb_connection* conn = sb_connect(dsn->c_str(), &err);
    ASSERT_NE(conn, nullptr) << err.message;

    sb_result* result = sb_query(conn, "SELECT 1", &err);
    ASSERT_NE(result, nullptr) << err.message;
    sb_result_free(result);
    ASSERT_EQ(sb_tx_savepoint(conn, "dmrw007_live_c", &err), SB_OK) << err.message;
    sb_disconnect(conn);

    conn = sb_connect(dsn->c_str(), &err);
    ASSERT_NE(conn, nullptr) << err.message;

    EXPECT_NE(sb_tx_rollback_to(conn, "dmrw007_live_c", &err), SB_OK);

    ASSERT_EQ(sb_tx_rollback(conn, &err), SB_OK) << err.message;
    ASSERT_EQ(sb_tx_savepoint(conn, "dmrw007_live_c_fresh", &err), SB_OK) << err.message;
    sb_disconnect(conn);
}
