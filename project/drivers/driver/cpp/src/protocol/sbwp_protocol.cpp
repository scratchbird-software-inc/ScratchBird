// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/protocol/sbwp_protocol.h"

#include <cstring>

namespace scratchbird::protocol {
namespace {
constexpr uint16_t kTxnFlagHasReadCommittedMode = 0x0100;

void setError(core::ErrorContext* ctx, const char* msg) {
    if (ctx) {
        ctx->message = msg ? msg : "";
    }
}

void writeU16(std::vector<uint8_t>& buf, size_t offset, uint16_t value) {
    buf[offset] = static_cast<uint8_t>(value & 0xFF);
    buf[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void writeU32(std::vector<uint8_t>& buf, size_t offset, uint32_t value) {
    buf[offset] = static_cast<uint8_t>(value & 0xFF);
    buf[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    buf[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    buf[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void writeU64(std::vector<uint8_t>& buf, size_t offset, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        buf[offset + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
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

uint64_t readU64(const uint8_t* data) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[i]) << (8 * i);
    }
    return value;
}

uint32_t oidFromCanonicalTypeRef(const std::vector<uint8_t>& payload, size_t offset) {
    if (offset + 4 > payload.size()) {
        return kOidText;
    }
    const uint16_t family = readU16(payload.data() + offset);
    const uint16_t code = readU16(payload.data() + offset + 2);
    if (family == 1 && code == 1) return kOidBool;
    if (family == 2 && code == 3) return kOidInt4;
    if (family == 2 && code == 4) return kOidInt8;
    if (family == 4 && code == 1) return kOidNumeric;
    if (family == 6 && code == 2) return kOidFloat8;
    if (family == 8 && code == 1) return kOidText;
    return kOidText;
}

int16_t typeSizeForOid(uint32_t oid) {
    switch (oid) {
        case kOidBool: return 1;
        case kOidInt4: return 4;
        case kOidInt8: return 8;
        case kOidFloat8: return 8;
        default: return -1;
    }
}

bool readNullableText(const std::vector<uint8_t>& payload,
                      size_t& offset,
                      std::string& value) {
    if (offset + 5 > payload.size()) {
        return false;
    }
    const uint8_t tag = payload[offset++];
    const uint32_t length = readU32(payload.data() + offset);
    offset += 4;
    if (tag == 0) {
        value.clear();
        return true;
    }
    if (offset + length > payload.size()) {
        return false;
    }
    value.assign(reinterpret_cast<const char*>(payload.data() + offset), length);
    offset += length;
    return true;
}

std::vector<uint8_t> buildParamList(const std::map<std::string, std::string>& params) {
    std::vector<uint8_t> buf;
    buf.reserve(128);
    for (const auto& entry : params) {
        buf.insert(buf.end(), entry.first.begin(), entry.first.end());
        buf.push_back(0);
        buf.insert(buf.end(), entry.second.begin(), entry.second.end());
        buf.push_back(0);
    }
    buf.push_back(0);
    return buf;
}

} // namespace

std::vector<uint8_t> encodeMessage(const MessageHeader& header,
                                   const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> buf;
    buf.resize(kHeaderSize + payload.size());
    writeU32(buf, 0, kProtocolMagic);
    buf[4] = kProtocolMajor;
    buf[5] = kProtocolMinor;
    buf[6] = static_cast<uint8_t>(header.type);
    buf[7] = header.flags;
    writeU32(buf, 8, static_cast<uint32_t>(payload.size()));
    writeU32(buf, 12, header.sequence);
    std::memcpy(buf.data() + 16, header.attachment_id.data(), header.attachment_id.size());
    writeU64(buf, 32, header.txn_id);
    if (!payload.empty()) {
        std::memcpy(buf.data() + kHeaderSize, payload.data(), payload.size());
    }
    return buf;
}

core::Status decodeHeader(const std::vector<uint8_t>& header_bytes,
                          MessageHeader& header,
                          core::ErrorContext* ctx) {
    if (header_bytes.size() != kHeaderSize) {
        setError(ctx, "Invalid header length");
        return core::Status::PROTOCOL_VIOLATION;
    }
    uint32_t magic = readU32(header_bytes.data());
    if (magic != kProtocolMagic) {
        setError(ctx, "Invalid protocol magic");
        return core::Status::PROTOCOL_VIOLATION;
    }
    if (header_bytes[4] != kProtocolMajor || header_bytes[5] != kProtocolMinor) {
        setError(ctx, "Unsupported protocol version");
        return core::Status::PROTOCOL_VIOLATION;
    }
    uint32_t length = readU32(header_bytes.data() + 8);
    if (length > kMaxMessageSize) {
        setError(ctx, "Payload too large");
        return core::Status::PROTOCOL_VIOLATION;
    }
    header.type = static_cast<MessageType>(header_bytes[6]);
    header.flags = header_bytes[7];
    header.length = length;
    header.sequence = readU32(header_bytes.data() + 12);
    std::memcpy(header.attachment_id.data(), header_bytes.data() + 16, header.attachment_id.size());
    header.txn_id = readU64(header_bytes.data() + 32);
    return core::Status::OK;
}

std::vector<uint8_t> buildStartupPayload(uint64_t features,
                                         const std::map<std::string, std::string>& params) {
    auto param_bytes = buildParamList(params);
    std::vector<uint8_t> payload(2 + 2 + 8 + param_bytes.size());
    payload[0] = kProtocolMajor;
    payload[1] = kProtocolMinor;
    writeU16(payload, 2, 0);
    writeU64(payload, 4, features);
    if (!param_bytes.empty()) {
        std::memcpy(payload.data() + 12, param_bytes.data(), param_bytes.size());
    }
    return payload;
}

namespace {

void appendU16(std::vector<uint8_t>& out, uint16_t value) {
    const size_t off = out.size();
    out.resize(off + 2);
    writeU16(out, off, value);
}

void appendU32(std::vector<uint8_t>& out, uint32_t value) {
    const size_t off = out.size();
    out.resize(off + 4);
    writeU32(out, off, value);
}

void appendU64(std::vector<uint8_t>& out, uint64_t value) {
    const size_t off = out.size();
    out.resize(off + 8);
    writeU64(out, off, value);
}

void appendLpString(std::vector<uint8_t>& out, const std::string& value) {
    appendU32(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

void appendU16String(std::vector<uint8_t>& out, const std::string& value) {
    appendU16(out, static_cast<uint16_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

bool readU16String(const std::vector<uint8_t>& payload,
                   size_t& offset,
                   std::string& value) {
    if (offset + 2 > payload.size()) {
        return false;
    }
    const uint16_t length = readU16(payload.data() + offset);
    offset += 2;
    if (offset + length > payload.size()) {
        return false;
    }
    value.assign(reinterpret_cast<const char*>(payload.data() + offset), length);
    offset += length;
    return true;
}

} // namespace

std::vector<uint8_t> buildP1StartupPayload(uint64_t client_features,
                                           uint64_t required_features,
                                           const std::map<std::string, std::string>& params) {
    std::vector<uint8_t> payload;
    appendU16(payload, kProtocolVersion);
    appendU16(payload, kProtocolVersion);
    appendU32(payload, 0);
    appendU64(payload, client_features);
    appendU64(payload, required_features);
    appendU64(payload, 0);
    payload.insert(payload.end(), 16, 0x11);
    payload.insert(payload.end(), 16, 0x00);
    payload.insert(payload.end(), 16, 0x00);
    appendU32(payload, static_cast<uint32_t>(params.size()));
    for (const auto& [key, value] : params) {
        appendLpString(payload, key);
        payload.push_back(0x01);
        payload.push_back(0x00);
        appendU32(payload, static_cast<uint32_t>(value.size()));
        payload.insert(payload.end(), value.begin(), value.end());
    }
    appendU32(payload, 0);
    return payload;
}

std::vector<uint8_t> buildQueryPayload(const std::string& query,
                                       uint32_t flags,
                                       uint32_t max_rows,
                                       uint32_t timeout_ms) {
    std::vector<uint8_t> query_bytes(query.begin(), query.end());
    query_bytes.push_back(0);
    std::vector<uint8_t> payload(4 + 4 + 4 + query_bytes.size());
    writeU32(payload, 0, flags);
    writeU32(payload, 4, max_rows);
    writeU32(payload, 8, timeout_ms);
    std::memcpy(payload.data() + 12, query_bytes.data(), query_bytes.size());
    return payload;
}

std::vector<uint8_t> buildParsePayload(const std::string& statement_name,
                                       const std::string& query,
                                       const std::vector<uint32_t>& param_types) {
    std::vector<uint8_t> name_bytes(statement_name.begin(), statement_name.end());
    std::vector<uint8_t> query_bytes(query.begin(), query.end());
    size_t payload_len = 4 + name_bytes.size() + 4 + query_bytes.size() + 2 + 2 + param_types.size() * 4;
    std::vector<uint8_t> payload(payload_len);
    size_t offset = 0;
    writeU32(payload, offset, static_cast<uint32_t>(name_bytes.size()));
    offset += 4;
    if (!name_bytes.empty()) {
        std::memcpy(payload.data() + offset, name_bytes.data(), name_bytes.size());
        offset += name_bytes.size();
    }
    writeU32(payload, offset, static_cast<uint32_t>(query_bytes.size()));
    offset += 4;
    if (!query_bytes.empty()) {
        std::memcpy(payload.data() + offset, query_bytes.data(), query_bytes.size());
        offset += query_bytes.size();
    }
    writeU16(payload, offset, static_cast<uint16_t>(param_types.size()));
    offset += 2;
    writeU16(payload, offset, 0);
    offset += 2;
    for (uint32_t oid : param_types) {
        writeU32(payload, offset, oid);
        offset += 4;
    }
    return payload;
}

std::vector<uint8_t> buildBindPayload(const std::string& portal_name,
                                      const std::string& statement_name,
                                      const std::vector<ParamValue>& params,
                                      const std::vector<uint16_t>& result_formats) {
    std::vector<uint8_t> portal_bytes(portal_name.begin(), portal_name.end());
    std::vector<uint8_t> stmt_bytes(statement_name.begin(), statement_name.end());
    std::vector<uint16_t> param_formats;
    param_formats.reserve(params.size());
    for (const auto& param : params) {
        param_formats.push_back(param.format);
    }
    size_t payload_len = 4 + portal_bytes.size() + 4 + stmt_bytes.size();
    payload_len += 2 + param_formats.size() * 2;
    payload_len += 2 + 2;
    for (const auto& param : params) {
        payload_len += 4;
        if (!param.is_null) {
            payload_len += param.data.size();
        }
    }
    payload_len += 2 + result_formats.size() * 2;

    std::vector<uint8_t> payload(payload_len);
    size_t offset = 0;
    writeU32(payload, offset, static_cast<uint32_t>(portal_bytes.size()));
    offset += 4;
    if (!portal_bytes.empty()) {
        std::memcpy(payload.data() + offset, portal_bytes.data(), portal_bytes.size());
        offset += portal_bytes.size();
    }
    writeU32(payload, offset, static_cast<uint32_t>(stmt_bytes.size()));
    offset += 4;
    if (!stmt_bytes.empty()) {
        std::memcpy(payload.data() + offset, stmt_bytes.data(), stmt_bytes.size());
        offset += stmt_bytes.size();
    }
    writeU16(payload, offset, static_cast<uint16_t>(param_formats.size()));
    offset += 2;
    for (uint16_t fmt : param_formats) {
        writeU16(payload, offset, fmt);
        offset += 2;
    }
    writeU16(payload, offset, static_cast<uint16_t>(params.size()));
    offset += 2;
    writeU16(payload, offset, 0);
    offset += 2;
    for (const auto& param : params) {
        if (param.is_null) {
            writeU32(payload, offset, 0xFFFFFFFFu);
            offset += 4;
            continue;
        }
        writeU32(payload, offset, static_cast<uint32_t>(param.data.size()));
        offset += 4;
        if (!param.data.empty()) {
            std::memcpy(payload.data() + offset, param.data.data(), param.data.size());
            offset += param.data.size();
        }
    }
    writeU16(payload, offset, static_cast<uint16_t>(result_formats.size()));
    offset += 2;
    for (uint16_t fmt : result_formats) {
        writeU16(payload, offset, fmt);
        offset += 2;
    }
    return payload;
}

std::vector<uint8_t> buildDescribePayload(uint8_t describe_type, const std::string& name) {
    std::vector<uint8_t> name_bytes(name.begin(), name.end());
    std::vector<uint8_t> payload(4 + 4 + name_bytes.size());
    payload[0] = describe_type;
    writeU32(payload, 4, static_cast<uint32_t>(name_bytes.size()));
    if (!name_bytes.empty()) {
        std::memcpy(payload.data() + 8, name_bytes.data(), name_bytes.size());
    }
    return payload;
}

std::vector<uint8_t> buildExecutePayload(const std::string& portal_name, uint32_t max_rows) {
    std::vector<uint8_t> portal_bytes(portal_name.begin(), portal_name.end());
    std::vector<uint8_t> payload(4 + portal_bytes.size() + 4);
    writeU32(payload, 0, static_cast<uint32_t>(portal_bytes.size()));
    if (!portal_bytes.empty()) {
        std::memcpy(payload.data() + 4, portal_bytes.data(), portal_bytes.size());
    }
    writeU32(payload, 4 + portal_bytes.size(), max_rows);
    return payload;
}

std::vector<uint8_t> buildSblrExecutePayload(uint64_t sblr_hash,
                                             const std::vector<uint8_t>& bytecode,
                                             const std::vector<ParamValue>& params) {
    size_t payload_len = 8 + 4 + 2 + 2 + bytecode.size();
    for (const auto& param : params) {
        payload_len += 4;
        if (!param.is_null) {
            payload_len += param.data.size();
        }
    }
    std::vector<uint8_t> payload(payload_len);
    size_t offset = 0;
    writeU64(payload, offset, sblr_hash);
    offset += 8;
    writeU32(payload, offset, static_cast<uint32_t>(bytecode.size()));
    offset += 4;
    writeU16(payload, offset, static_cast<uint16_t>(params.size()));
    offset += 2;
    writeU16(payload, offset, 0);
    offset += 2;
    if (!bytecode.empty()) {
        std::memcpy(payload.data() + offset, bytecode.data(), bytecode.size());
        offset += bytecode.size();
    }
    for (const auto& param : params) {
        if (param.is_null) {
            writeU32(payload, offset, 0xFFFFFFFFu);
            offset += 4;
            continue;
        }
        writeU32(payload, offset, static_cast<uint32_t>(param.data.size()));
        offset += 4;
        if (!param.data.empty()) {
            std::memcpy(payload.data() + offset, param.data.data(), param.data.size());
            offset += param.data.size();
        }
    }
    return payload;
}

std::vector<uint8_t> buildSubscribePayload(uint8_t subscribe_type,
                                           const std::string& channel,
                                           const std::string& filter) {
    std::vector<uint8_t> channel_bytes(channel.begin(), channel.end());
    std::vector<uint8_t> filter_bytes(filter.begin(), filter.end());
    size_t payload_len = 4 + 4 + channel_bytes.size() + 4 + filter_bytes.size();
    std::vector<uint8_t> payload(payload_len);
    payload[0] = subscribe_type;
    size_t offset = 4;
    writeU32(payload, offset, static_cast<uint32_t>(channel_bytes.size()));
    offset += 4;
    if (!channel_bytes.empty()) {
        std::memcpy(payload.data() + offset, channel_bytes.data(), channel_bytes.size());
        offset += channel_bytes.size();
    }
    writeU32(payload, offset, static_cast<uint32_t>(filter_bytes.size()));
    offset += 4;
    if (!filter_bytes.empty()) {
        std::memcpy(payload.data() + offset, filter_bytes.data(), filter_bytes.size());
    }
    return payload;
}

std::vector<uint8_t> buildUnsubscribePayload(const std::string& channel) {
    std::vector<uint8_t> channel_bytes(channel.begin(), channel.end());
    std::vector<uint8_t> payload(4 + channel_bytes.size());
    writeU32(payload, 0, static_cast<uint32_t>(channel_bytes.size()));
    if (!channel_bytes.empty()) {
        std::memcpy(payload.data() + 4, channel_bytes.data(), channel_bytes.size());
    }
    return payload;
}

std::vector<uint8_t> buildTxnBeginPayload(uint16_t flags,
                                          uint8_t conflict_action,
                                          uint8_t autocommit_mode,
                                          uint8_t isolation_level,
                                          uint8_t access_mode,
                                          uint8_t deferrable,
                                          uint8_t wait_mode,
                                          uint32_t timeout_ms,
                                          uint8_t read_committed_mode) {
    const bool has_read_committed_mode = (flags & kTxnFlagHasReadCommittedMode) != 0;
    std::vector<uint8_t> payload(has_read_committed_mode ? 16 : 12);
    writeU16(payload, 0, flags);
    payload[2] = conflict_action;
    payload[3] = autocommit_mode;
    payload[4] = isolation_level;
    payload[5] = access_mode;
    payload[6] = deferrable;
    payload[7] = wait_mode;
    writeU32(payload, 8, timeout_ms);
    if (has_read_committed_mode) {
        payload[12] = read_committed_mode;
    }
    return payload;
}

std::vector<uint8_t> buildTxnCommitPayload(uint8_t flags) {
    std::vector<uint8_t> payload(4);
    payload[0] = flags;
    return payload;
}

std::vector<uint8_t> buildTxnCommitPayload(const TxnCommitRequest& request) {
    std::vector<uint8_t> payload(36);
    payload[0] = request.legacy_flags;
    payload[1] = kTxnFinalityPayloadVersion;
    writeU16(payload, 2, request.contract_flags);
    std::memcpy(payload.data() + 4,
                request.idempotency_key.data(),
                request.idempotency_key.size());
    writeU64(payload, 20, request.request_fingerprint);
    writeU64(payload, 28, request.expected_txn_id);
    return payload;
}

std::vector<uint8_t> buildTxnRollbackPayload(uint8_t flags) {
    std::vector<uint8_t> payload(4);
    payload[0] = flags;
    return payload;
}

std::vector<uint8_t> buildTxnFinalityQueryPayload(const TxnFinalityQuery& query) {
    std::vector<uint8_t> payload(44);
    payload[0] = kTxnFinalityPayloadVersion;
    payload[1] = 0;
    writeU16(payload, 2, query.flags);
    std::memcpy(payload.data() + 4,
                query.idempotency_key.data(),
                query.idempotency_key.size());
    std::memcpy(payload.data() + 20,
                query.finality_token.data(),
                query.finality_token.size());
    writeU64(payload, 36, query.expected_txn_id);
    return payload;
}

std::vector<uint8_t> buildTxnFinalityStatusPayload(const TxnFinalityStatus& status) {
    std::vector<uint8_t> payload;
    payload.reserve(64 + status.diagnostic_code.size() + status.detail.size());
    payload.push_back(kTxnFinalityPayloadVersion);
    payload.push_back(static_cast<uint8_t>(status.state));
    appendU16(payload, status.flags);
    payload.insert(payload.end(),
                   status.idempotency_key.begin(),
                   status.idempotency_key.end());
    payload.insert(payload.end(),
                   status.finality_token.begin(),
                   status.finality_token.end());
    appendU64(payload, status.request_fingerprint);
    appendU64(payload, status.original_txn_id);
    appendU64(payload, status.replacement_txn_id);
    appendU16String(payload, status.diagnostic_code);
    appendU16String(payload, status.detail);
    return payload;
}

std::vector<uint8_t> buildTxnSavepointPayload(const std::string& name) {
    std::vector<uint8_t> name_bytes(name.begin(), name.end());
    std::vector<uint8_t> payload(4 + name_bytes.size());
    writeU32(payload, 0, static_cast<uint32_t>(name_bytes.size()));
    if (!name_bytes.empty()) {
        std::memcpy(payload.data() + 4, name_bytes.data(), name_bytes.size());
    }
    return payload;
}

std::vector<uint8_t> buildTxnReleasePayload(const std::string& name) {
    return buildTxnSavepointPayload(name);
}

std::vector<uint8_t> buildTxnRollbackToPayload(const std::string& name) {
    return buildTxnSavepointPayload(name);
}

std::vector<uint8_t> buildSetOptionPayload(const std::string& name, const std::string& value) {
    std::vector<uint8_t> name_bytes(name.begin(), name.end());
    std::vector<uint8_t> value_bytes(value.begin(), value.end());
    std::vector<uint8_t> payload(4 + name_bytes.size() + 4 + value_bytes.size());
    writeU32(payload, 0, static_cast<uint32_t>(name_bytes.size()));
    if (!name_bytes.empty()) {
        std::memcpy(payload.data() + 4, name_bytes.data(), name_bytes.size());
    }
    size_t offset = 4 + name_bytes.size();
    writeU32(payload, offset, static_cast<uint32_t>(value_bytes.size()));
    offset += 4;
    if (!value_bytes.empty()) {
        std::memcpy(payload.data() + offset, value_bytes.data(), value_bytes.size());
    }
    return payload;
}

std::vector<uint8_t> buildStreamControlPayload(uint8_t control_type,
                                               uint32_t window_size,
                                               uint32_t timeout_ms) {
    std::vector<uint8_t> payload(12);
    payload[0] = control_type;
    writeU32(payload, 4, window_size);
    writeU32(payload, 8, timeout_ms);
    return payload;
}

std::vector<uint8_t> buildAttachCreatePayload(const std::string& mode,
                                              const std::string& db_name) {
    std::vector<uint8_t> mode_bytes(mode.begin(), mode.end());
    std::vector<uint8_t> db_bytes(db_name.begin(), db_name.end());
    std::vector<uint8_t> payload(4 + mode_bytes.size() + 4 + db_bytes.size());
    writeU32(payload, 0, static_cast<uint32_t>(mode_bytes.size()));
    if (!mode_bytes.empty()) {
        std::memcpy(payload.data() + 4, mode_bytes.data(), mode_bytes.size());
    }
    size_t offset = 4 + mode_bytes.size();
    writeU32(payload, offset, static_cast<uint32_t>(db_bytes.size()));
    offset += 4;
    if (!db_bytes.empty()) {
        std::memcpy(payload.data() + offset, db_bytes.data(), db_bytes.size());
    }
    return payload;
}

std::vector<uint8_t> buildAttachDetachPayload() {
    return {};
}

std::vector<uint8_t> buildAttachListPayload() {
    return {};
}

std::vector<uint8_t> buildClosePayload(uint8_t close_type, const std::string& name) {
    std::vector<uint8_t> name_bytes(name.begin(), name.end());
    std::vector<uint8_t> payload(4 + 4 + name_bytes.size());
    payload[0] = close_type;
    writeU32(payload, 4, static_cast<uint32_t>(name_bytes.size()));
    if (!name_bytes.empty()) {
        std::memcpy(payload.data() + 8, name_bytes.data(), name_bytes.size());
    }
    return payload;
}

std::vector<uint8_t> buildCancelPayload(uint32_t cancel_type, uint32_t target_seq) {
    std::vector<uint8_t> payload(8);
    writeU32(payload, 0, cancel_type);
    writeU32(payload, 4, target_seq);
    return payload;
}

std::vector<uint8_t> buildCopyDataPayload(const std::vector<uint8_t>& data) {
    return data;
}

std::vector<uint8_t> buildCopyDonePayload() {
    return {};
}

std::vector<uint8_t> buildCopyFailPayload(const std::string& error_message) {
    std::vector<uint8_t> payload(4 + error_message.size());
    writeU32(payload, 0, static_cast<uint32_t>(error_message.size()));
    if (!error_message.empty()) {
        std::memcpy(payload.data() + 4, error_message.data(), error_message.size());
    }
    return payload;
}

std::vector<uint8_t> buildCopyInResponsePayload(uint8_t format, uint32_t window_bytes) {
    std::vector<uint8_t> payload(5);
    payload[0] = format;
    writeU32(payload, 1, window_bytes);
    return payload;
}

std::vector<uint8_t> buildCopyOutResponsePayload(uint8_t format,
                                                 const std::vector<uint32_t>& column_formats) {
    std::vector<uint8_t> payload(3 + column_formats.size() * 4);
    payload[0] = format;
    writeU16(payload, 1, static_cast<uint16_t>(column_formats.size()));
    size_t offset = 3;
    for (uint32_t column_format : column_formats) {
        writeU32(payload, offset, column_format);
        offset += 4;
    }
    return payload;
}

std::vector<uint8_t> buildCopyBothResponsePayload(uint8_t format, uint32_t window_bytes) {
    return buildCopyInResponsePayload(format, window_bytes);
}

core::Status parseAuthRequest(const std::vector<uint8_t>& payload,
                              AuthMethod& method,
                              std::vector<uint8_t>& data,
                              core::ErrorContext* ctx) {
    if (payload.size() < 4) {
        setError(ctx, "Auth request truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    method = static_cast<AuthMethod>(payload[0]);
    data.assign(payload.begin() + 4, payload.end());
    return core::Status::OK;
}

core::Status parseAuthContinue(const std::vector<uint8_t>& payload,
                               AuthMethod& method,
                               uint8_t& stage,
                               std::vector<uint8_t>& data,
                               core::ErrorContext* ctx) {
    if (payload.size() < 8) {
        setError(ctx, "Auth continue truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    method = static_cast<AuthMethod>(payload[0]);
    stage = payload[1];
    uint32_t data_len = readU32(payload.data() + 4);
    if (8u + data_len > payload.size()) {
        setError(ctx, "Auth continue truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    data.assign(payload.begin() + 8, payload.begin() + 8 + data_len);
    return core::Status::OK;
}

core::Status parseAuthOk(const std::vector<uint8_t>& payload,
                         std::vector<uint8_t>& session_id,
                         std::vector<uint8_t>& info,
                         core::ErrorContext* ctx) {
    if (payload.size() < 20) {
        setError(ctx, "Auth ok truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    session_id.assign(payload.begin(), payload.begin() + 16);
    uint32_t info_len = readU32(payload.data() + 16);
    if (20u + info_len > payload.size()) {
        setError(ctx, "Auth ok truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    info.assign(payload.begin() + 20, payload.begin() + 20 + info_len);
    return core::Status::OK;
}

core::Status parseReady(const std::vector<uint8_t>& payload,
                        uint8_t& status,
                        uint64_t& txn_id,
                        uint64_t& epoch,
                        core::ErrorContext* ctx) {
    if (payload.size() >= 76 &&
        (payload[56] == 0x49 || payload[56] == 0x54 || payload[56] == 0x45 ||
         payload[56] == 0x52 || payload[56] == 0x41)) {
        txn_id = readU64(payload.data() + 48);
        status = (payload[56] == 0x54 || payload[56] == 0x45) ? 1 : 0;
        epoch = txn_id;
        return core::Status::OK;
    }
    if (payload.size() < 20) {
        setError(ctx, "Ready truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    status = payload[0];
    txn_id = readU64(payload.data() + 4);
    epoch = readU64(payload.data() + 12);
    return core::Status::OK;
}

core::Status parseParameterStatus(const std::vector<uint8_t>& payload,
                                  std::string& name,
                                  std::string& value,
                                  core::ErrorContext* ctx) {
    if (payload.size() < 8) {
        setError(ctx, "Parameter status truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    size_t offset = 0;
    uint32_t name_len = readU32(payload.data() + offset);
    offset += 4;
    if (offset + name_len + 4 > payload.size()) {
        setError(ctx, "Parameter status truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    name.assign(reinterpret_cast<const char*>(payload.data() + offset), name_len);
    offset += name_len;
    uint32_t value_len = readU32(payload.data() + offset);
    offset += 4;
    if (offset + value_len > payload.size()) {
        setError(ctx, "Parameter status truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    value.assign(reinterpret_cast<const char*>(payload.data() + offset), value_len);
    return core::Status::OK;
}

core::Status parseParameterStatuses(const std::vector<uint8_t>& payload,
                                    std::vector<std::pair<std::string, std::string>>& values,
                                    core::ErrorContext* ctx) {
    values.clear();
    if (payload.size() < 8) {
        setError(ctx, "Parameter status truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    const uint32_t count = readU32(payload.data());
    if (count > 0 && count <= 256) {
        size_t offset = 4;
        std::vector<std::pair<std::string, std::string>> parsed;
        parsed.reserve(count);
        bool ok = true;
        for (uint32_t i = 0; i < count; ++i) {
            if (offset + 4 > payload.size()) {
                ok = false;
                break;
            }
            uint32_t name_len = readU32(payload.data() + offset);
            offset += 4;
            if (offset + name_len + 7 > payload.size()) {
                ok = false;
                break;
            }
            std::string name(reinterpret_cast<const char*>(payload.data() + offset), name_len);
            offset += name_len;
            offset += 3;
            uint32_t value_len = readU32(payload.data() + offset);
            offset += 4;
            if (offset + value_len > payload.size()) {
                ok = false;
                break;
            }
            std::string value(reinterpret_cast<const char*>(payload.data() + offset), value_len);
            offset += value_len;
            parsed.emplace_back(std::move(name), std::move(value));
        }
        if (ok && offset == payload.size()) {
            values = std::move(parsed);
            return core::Status::OK;
        }
    }

    std::string name;
    std::string value;
    auto status = parseParameterStatus(payload, name, value, ctx);
    if (status == core::Status::OK) {
        values.emplace_back(std::move(name), std::move(value));
    }
    return status;
}

core::Status parseParameterDescription(const std::vector<uint8_t>& payload,
                                       std::vector<uint32_t>& param_types,
                                       core::ErrorContext* ctx) {
    if (payload.size() < 4) {
        setError(ctx, "Parameter description truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    uint16_t count = readU16(payload.data());
    size_t offset = 4;
    param_types.clear();
    param_types.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        if (offset + 4 > payload.size()) {
            setError(ctx, "Parameter description truncated");
            return core::Status::PROTOCOL_VIOLATION;
        }
        param_types.push_back(readU32(payload.data() + offset));
        offset += 4;
    }
    return core::Status::OK;
}

core::Status parseRowDescription(const std::vector<uint8_t>& payload,
                                 std::vector<ColumnInfo>& columns,
                                 core::ErrorContext* ctx) {
    if (payload.size() >= 72 && readU16(payload.data()) == 1 && payload[3] == 1) {
        const uint32_t count = readU32(payload.data() + 4);
        size_t offset = 72;
        columns.clear();
        columns.reserve(count);
        constexpr size_t kCanonicalTypeRefSize = 144;
        for (uint32_t i = 0; i < count; ++i) {
            if (offset + 4 + 4 + 8 + kCanonicalTypeRefSize + 56 > payload.size()) {
                setError(ctx, "P1 row description truncated");
                return core::Status::PROTOCOL_VIOLATION;
            }
            ColumnInfo info;
            const uint32_t ordinal = readU32(payload.data() + offset);
            info.column_index = static_cast<uint16_t>(ordinal == 0 ? i : ordinal - 1);
            offset += 4;
            offset += 1;
            info.format = payload[offset++] == 1 ? 0 : 1;
            info.nullable = payload[offset++] == 1;
            offset += 1;
            offset += 8;
            info.type_oid = oidFromCanonicalTypeRef(payload, offset);
            info.type_size = typeSizeForOid(info.type_oid);
            info.type_modifier = -1;
            offset += kCanonicalTypeRefSize;
            offset += 16 * 3;
            offset += 4;
            offset += 2;
            offset += 2;
            if (!readNullableText(payload, offset, info.name)) {
                setError(ctx, "P1 row description truncated");
                return core::Status::PROTOCOL_VIOLATION;
            }
            if (info.name.empty()) {
                info.name = "column" + std::to_string(columns.size() + 1);
            }
            columns.push_back(std::move(info));
        }
        return core::Status::OK;
    }
    if (payload.size() < 4) {
        setError(ctx, "Row description truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    uint16_t count = readU16(payload.data());
    size_t offset = 4;
    columns.clear();
    columns.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        if (offset + 4 > payload.size()) {
            setError(ctx, "Row description truncated");
            return core::Status::PROTOCOL_VIOLATION;
        }
        uint32_t name_len = readU32(payload.data() + offset);
        offset += 4;
        if (offset + name_len + 4 + 2 + 4 + 2 + 4 + 1 + 1 + 2 > payload.size()) {
            setError(ctx, "Row description truncated");
            return core::Status::PROTOCOL_VIOLATION;
        }
        ColumnInfo info;
        info.name.assign(reinterpret_cast<const char*>(payload.data() + offset), name_len);
        offset += name_len;
        info.table_oid = readU32(payload.data() + offset);
        offset += 4;
        info.column_index = readU16(payload.data() + offset);
        offset += 2;
        info.type_oid = readU32(payload.data() + offset);
        offset += 4;
        info.type_size = static_cast<int16_t>(readU16(payload.data() + offset));
        offset += 2;
        info.type_modifier = static_cast<int32_t>(readU32(payload.data() + offset));
        offset += 4;
        info.format = payload[offset++];
        info.nullable = payload[offset++] == 1;
        offset += 2;
        columns.push_back(std::move(info));
    }
    return core::Status::OK;
}

core::Status parseDataRow(const std::vector<uint8_t>& payload,
                          size_t column_count,
                          std::vector<ColumnValue>& values,
                          core::ErrorContext* ctx) {
    if (payload.size() < 4) {
        setError(ctx, "Row data truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    size_t offset = 0;
    uint16_t count = readU16(payload.data());
    offset += 2;
    uint16_t null_bytes = readU16(payload.data() + offset);
    offset += 2;
    if (count != column_count) {
        setError(ctx, "Row data column count mismatch");
        return core::Status::PROTOCOL_VIOLATION;
    }
    if (offset + null_bytes > payload.size()) {
        setError(ctx, "Row data truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    const uint8_t* null_bitmap = payload.data() + offset;
    offset += null_bytes;
    values.clear();
    values.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        size_t byte_index = i / 8;
        uint8_t bit_index = static_cast<uint8_t>(i % 8);
        bool is_null = byte_index < null_bytes && (null_bitmap[byte_index] & (1u << bit_index)) != 0;
        if (is_null) {
            values.push_back(ColumnValue{{}, true});
            continue;
        }
        if (offset + 4 > payload.size()) {
            setError(ctx, "Row data truncated");
            return core::Status::PROTOCOL_VIOLATION;
        }
        uint32_t length = readU32(payload.data() + offset);
        offset += 4;
        if (length == 0xFFFFFFFFu) {
            values.push_back(ColumnValue{{}, true});
            continue;
        }
        if (offset + length > payload.size()) {
            setError(ctx, "Row data truncated");
            return core::Status::PROTOCOL_VIOLATION;
        }
        ColumnValue value;
        value.is_null = false;
        value.data.assign(payload.begin() + offset, payload.begin() + offset + length);
        offset += length;
        values.push_back(std::move(value));
    }
    return core::Status::OK;
}

core::Status parseCommandComplete(const std::vector<uint8_t>& payload,
                                  uint8_t& command_type,
                                  uint64_t& rows,
                                  uint64_t& last_id,
                                  std::string& tag,
                                  core::ErrorContext* ctx) {
    if (payload.size() < 20) {
        setError(ctx, "Command complete truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    command_type = payload[0];
    rows = readU64(payload.data() + 4);
    last_id = readU64(payload.data() + 12);
    tag.assign(reinterpret_cast<const char*>(payload.data() + 20), payload.size() - 20);
    for (size_t i = 0; i < tag.size(); ++i) {
        if (tag[i] == '\0') {
            tag.resize(i);
            break;
        }
    }
    return core::Status::OK;
}

core::Status parseTxnCommitRequest(const std::vector<uint8_t>& payload,
                                   TxnCommitRequest& request,
                                   core::ErrorContext* ctx) {
    request = TxnCommitRequest{};
    if (payload.size() < 4) {
        setError(ctx, "TxnCommit payload truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    request.legacy_flags = payload[0];
    if (payload.size() == 4 && payload[1] == 0 && payload[2] == 0 && payload[3] == 0) {
        return core::Status::OK;
    }
    if (payload.size() < 36) {
        setError(ctx, "TxnCommit finality payload truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    if (payload[1] != kTxnFinalityPayloadVersion) {
        setError(ctx, "Unsupported TxnCommit finality payload version");
        return core::Status::PROTOCOL_VIOLATION;
    }
    request.contract_flags = readU16(payload.data() + 2);
    std::memcpy(request.idempotency_key.data(),
                payload.data() + 4,
                request.idempotency_key.size());
    request.request_fingerprint = readU64(payload.data() + 20);
    request.expected_txn_id = readU64(payload.data() + 28);
    return core::Status::OK;
}

core::Status parseTxnFinalityQuery(const std::vector<uint8_t>& payload,
                                   TxnFinalityQuery& query,
                                   core::ErrorContext* ctx) {
    query = TxnFinalityQuery{};
    if (payload.size() < 44) {
        setError(ctx, "TxnStatus query payload truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    if (payload[0] != kTxnFinalityPayloadVersion) {
        setError(ctx, "Unsupported TxnStatus query payload version");
        return core::Status::PROTOCOL_VIOLATION;
    }
    query.flags = readU16(payload.data() + 2);
    std::memcpy(query.idempotency_key.data(),
                payload.data() + 4,
                query.idempotency_key.size());
    std::memcpy(query.finality_token.data(),
                payload.data() + 20,
                query.finality_token.size());
    query.expected_txn_id = readU64(payload.data() + 36);
    return core::Status::OK;
}

core::Status parseTxnFinalityStatus(const std::vector<uint8_t>& payload,
                                    TxnFinalityStatus& status,
                                    core::ErrorContext* ctx) {
    status = TxnFinalityStatus{};
    if (payload.size() < 60) {
        setError(ctx, "TxnStatus payload truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    if (payload[0] != kTxnFinalityPayloadVersion) {
        setError(ctx, "Unsupported TxnStatus payload version");
        return core::Status::PROTOCOL_VIOLATION;
    }
    status.state = static_cast<TxnFinalityState>(payload[1]);
    status.flags = readU16(payload.data() + 2);
    std::memcpy(status.idempotency_key.data(),
                payload.data() + 4,
                status.idempotency_key.size());
    std::memcpy(status.finality_token.data(),
                payload.data() + 20,
                status.finality_token.size());
    status.request_fingerprint = readU64(payload.data() + 36);
    status.original_txn_id = readU64(payload.data() + 44);
    status.replacement_txn_id = readU64(payload.data() + 52);
    size_t offset = 60;
    if (!readU16String(payload, offset, status.diagnostic_code) ||
        !readU16String(payload, offset, status.detail)) {
        setError(ctx, "TxnStatus string fields truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    return core::Status::OK;
}

core::Status parseNotification(const std::vector<uint8_t>& payload,
                               Notification& notice,
                               core::ErrorContext* ctx) {
    if (payload.size() < 12) {
        setError(ctx, "Notification truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    size_t offset = 0;
    notice.process_id = readU32(payload.data());
    offset += 4;
    uint32_t channel_len = readU32(payload.data() + offset);
    offset += 4;
    if (offset + channel_len + 4 > payload.size()) {
        setError(ctx, "Notification truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    notice.channel.assign(reinterpret_cast<const char*>(payload.data() + offset), channel_len);
    offset += channel_len;
    uint32_t payload_len = readU32(payload.data() + offset);
    offset += 4;
    if (offset + payload_len > payload.size()) {
        setError(ctx, "Notification truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    notice.payload.assign(payload.begin() + offset, payload.begin() + offset + payload_len);
    offset += payload_len;
    notice.change_type = 0;
    notice.row_id = 0;
    notice.has_row = false;
    if (offset < payload.size()) {
        notice.change_type = payload[offset++];
        if (offset + 8 <= payload.size()) {
            notice.row_id = readU64(payload.data() + offset);
            notice.has_row = true;
        }
    }
    return core::Status::OK;
}

core::Status parseQueryPlan(const std::vector<uint8_t>& payload,
                            QueryPlan& plan,
                            core::ErrorContext* ctx) {
    if (payload.size() < 32) {
        setError(ctx, "Query plan truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    plan.format = readU32(payload.data());
    uint32_t plan_len = readU32(payload.data() + 4);
    plan.planning_time = readU64(payload.data() + 8);
    plan.estimated_rows = readU64(payload.data() + 16);
    plan.estimated_cost = readU64(payload.data() + 24);
    if (32u + plan_len > payload.size()) {
        setError(ctx, "Query plan truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    plan.plan.assign(payload.begin() + 32, payload.begin() + 32 + plan_len);
    return core::Status::OK;
}

core::Status parseSblrCompiled(const std::vector<uint8_t>& payload,
                               SblrCompiled& compiled,
                               core::ErrorContext* ctx) {
    if (payload.size() < 16) {
        setError(ctx, "SBLR compiled truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    compiled.hash = readU64(payload.data());
    compiled.version = readU32(payload.data() + 8);
    uint32_t length = readU32(payload.data() + 12);
    if (16u + length > payload.size()) {
        setError(ctx, "SBLR compiled truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    compiled.bytecode.assign(payload.begin() + 16, payload.begin() + 16 + length);
    return core::Status::OK;
}

core::Status parseCopyInResponse(const std::vector<uint8_t>& payload,
                                 CopyInResponse& response,
                                 core::ErrorContext* ctx) {
    if (payload.size() < 5) {
        setError(ctx, "Copy in response truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    response.format = payload[0];
    response.window_bytes = readU32(payload.data() + 1);
    return core::Status::OK;
}

core::Status parseCopyOutResponse(const std::vector<uint8_t>& payload,
                                  CopyOutResponse& response,
                                  core::ErrorContext* ctx) {
    if (payload.size() < 3) {
        setError(ctx, "Copy out response truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    response.format = payload[0];
    response.column_count = readU16(payload.data() + 1);
    size_t offset = 3;
    response.column_formats.clear();
    response.column_formats.reserve(response.column_count);
    for (uint16_t i = 0; i < response.column_count; ++i) {
        if (offset + 4 > payload.size()) {
            setError(ctx, "Copy out response truncated");
            return core::Status::PROTOCOL_VIOLATION;
        }
        response.column_formats.push_back(readU32(payload.data() + offset));
        offset += 4;
    }
    return core::Status::OK;
}

core::Status parseCopyBothResponse(const std::vector<uint8_t>& payload,
                                   CopyBothResponse& response,
                                   core::ErrorContext* ctx) {
    CopyInResponse in_response;
    auto status = parseCopyInResponse(payload, in_response, ctx);
    if (status != core::Status::OK) {
        return status;
    }
    response.format = in_response.format;
    response.window_bytes = in_response.window_bytes;
    return core::Status::OK;
}

core::Status parseCopyData(const std::vector<uint8_t>& payload,
                           CopyData& data,
                           core::ErrorContext* /*ctx*/) {
    data.data = payload;
    return core::Status::OK;
}

core::Status parseCopyFail(const std::vector<uint8_t>& payload,
                           CopyFailInfo& fail,
                           core::ErrorContext* ctx) {
    if (payload.size() < 4) {
        setError(ctx, "Copy fail truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    const uint32_t length = readU32(payload.data());
    if (4u + length > payload.size()) {
        setError(ctx, "Copy fail truncated");
        return core::Status::PROTOCOL_VIOLATION;
    }
    fail.error_message.assign(reinterpret_cast<const char*>(payload.data() + 4), length);
    return core::Status::OK;
}

core::Status parseErrorMessage(const std::vector<uint8_t>& payload,
                               std::string& severity,
                               std::string& sqlstate,
                               std::string& message,
                               std::string& detail,
                               std::string& hint,
                               core::ErrorContext* ctx) {
    severity.clear();
    sqlstate.clear();
    message.clear();
    detail.clear();
    hint.clear();
    size_t offset = 0;
    while (offset < payload.size()) {
        uint8_t field = payload[offset++];
        if (field == 0) {
            break;
        }
        size_t start = offset;
        while (offset < payload.size() && payload[offset] != 0) {
            ++offset;
        }
        if (offset >= payload.size()) {
            setError(ctx, "Error message truncated");
            return core::Status::PROTOCOL_VIOLATION;
        }
        std::string value(reinterpret_cast<const char*>(payload.data() + start), offset - start);
        ++offset;
        switch (field) {
            case 'S': severity = value; break;
            case 'C': sqlstate = value; break;
            case 'M': message = value; break;
            case 'D': detail = value; break;
            case 'H': hint = value; break;
            default: break;
        }
    }
    return core::Status::OK;
}

} // namespace scratchbird::protocol
