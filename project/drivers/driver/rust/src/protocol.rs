// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

use std::collections::HashMap;

use crate::errors::{Error, ErrorKind, Result};

pub const MAGIC_BYTES: [u8; 4] = *b"SBWP";
pub const VERSION_MAJOR: u8 = 1;
pub const VERSION_MINOR: u8 = 1;
pub const HEADER_SIZE: usize = 40;
pub const MAX_MESSAGE_SIZE: u32 = 1024 * 1024 * 1024;

pub const MSG_STARTUP: u8 = 0x01;
pub const MSG_AUTH_RESPONSE: u8 = 0x02;
pub const MSG_QUERY: u8 = 0x03;
pub const MSG_PARSE: u8 = 0x04;
pub const MSG_BIND: u8 = 0x05;
pub const MSG_DESCRIBE: u8 = 0x06;
pub const MSG_EXECUTE: u8 = 0x07;
pub const MSG_CLOSE: u8 = 0x08;
pub const MSG_SYNC: u8 = 0x09;
pub const MSG_FLUSH: u8 = 0x0A;
pub const MSG_CANCEL: u8 = 0x0B;
pub const MSG_TERMINATE: u8 = 0x0C;
pub const MSG_COPY_DATA: u8 = 0x0D;
pub const MSG_COPY_DONE: u8 = 0x0E;
pub const MSG_COPY_FAIL: u8 = 0x0F;
pub const MSG_SBLR_EXECUTE: u8 = 0x10;
pub const MSG_SUBSCRIBE: u8 = 0x11;
pub const MSG_UNSUBSCRIBE: u8 = 0x12;
pub const MSG_FEDERATED_QUERY: u8 = 0x13;
pub const MSG_STREAM_CONTROL: u8 = 0x14;
pub const MSG_TXN_BEGIN: u8 = 0x15;
pub const MSG_TXN_COMMIT: u8 = 0x16;
pub const MSG_TXN_ROLLBACK: u8 = 0x17;
pub const MSG_TXN_SAVEPOINT: u8 = 0x18;
pub const MSG_TXN_RELEASE: u8 = 0x19;
pub const MSG_TXN_ROLLBACK_TO: u8 = 0x1A;
pub const MSG_PING: u8 = 0x1B;
pub const MSG_SET_OPTION: u8 = 0x1C;
pub const MSG_CLUSTER_AUTH: u8 = 0x1D;
pub const MSG_ATTACH_CREATE: u8 = 0x1E;
pub const MSG_ATTACH_DETACH: u8 = 0x1F;
pub const MSG_ATTACH_LIST: u8 = 0x20;

pub const MSG_AUTH_REQUEST: u8 = 0x40;
pub const MSG_AUTH_OK: u8 = 0x41;
pub const MSG_AUTH_CONTINUE: u8 = 0x42;
pub const MSG_READY: u8 = 0x43;
pub const MSG_ROW_DESCRIPTION: u8 = 0x44;
pub const MSG_DATA_ROW: u8 = 0x45;
pub const MSG_COMMAND_COMPLETE: u8 = 0x46;
pub const MSG_EMPTY_QUERY: u8 = 0x47;
pub const MSG_ERROR: u8 = 0x48;
pub const MSG_NOTICE: u8 = 0x49;
pub const MSG_PARSE_COMPLETE: u8 = 0x4A;
pub const MSG_BIND_COMPLETE: u8 = 0x4B;
pub const MSG_CLOSE_COMPLETE: u8 = 0x4C;
pub const MSG_PORTAL_SUSPENDED: u8 = 0x4D;
pub const MSG_NO_DATA: u8 = 0x4E;
pub const MSG_PARAMETER_STATUS: u8 = 0x4F;
pub const MSG_PARAMETER_DESCRIPTION: u8 = 0x50;
pub const MSG_COPY_IN_RESPONSE: u8 = 0x51;
pub const MSG_COPY_OUT_RESPONSE: u8 = 0x52;
pub const MSG_COPY_BOTH_RESPONSE: u8 = 0x53;
pub const MSG_NOTIFICATION: u8 = 0x54;
pub const MSG_FUNCTION_RESULT: u8 = 0x55;
pub const MSG_NEGOTIATE_VERSION: u8 = 0x56;
pub const MSG_SBLR_COMPILED: u8 = 0x57;
pub const MSG_QUERY_PLAN: u8 = 0x58;
pub const MSG_STREAM_READY: u8 = 0x59;
pub const MSG_STREAM_DATA: u8 = 0x5A;
pub const MSG_STREAM_END: u8 = 0x5B;
pub const MSG_TXN_STATUS: u8 = 0x5C;
pub const MSG_PONG: u8 = 0x5D;
pub const MSG_CLUSTER_AUTH_OK: u8 = 0x5E;
pub const MSG_FEDERATED_RESULT: u8 = 0x5F;
pub const MSG_HEARTBEAT: u8 = 0x80;
pub const MSG_EXTENSION: u8 = 0x81;

pub const MSG_FLAG_COMPRESSED: u8 = 0x01;
pub const MSG_FLAG_CONTINUED: u8 = 0x02;
pub const MSG_FLAG_FINAL: u8 = 0x04;
pub const MSG_FLAG_URGENT: u8 = 0x08;
pub const MSG_FLAG_ENCRYPTED: u8 = 0x10;
pub const MSG_FLAG_CHECKSUM: u8 = 0x20;

pub const FEATURE_COMPRESSION: u64 = 1 << 0;
pub const FEATURE_STREAMING: u64 = 1 << 1;
pub const FEATURE_SBLR: u64 = 1 << 2;
pub const FEATURE_FEDERATION: u64 = 1 << 3;
pub const FEATURE_NOTIFICATIONS: u64 = 1 << 4;
pub const FEATURE_QUERY_PLAN: u64 = 1 << 5;
pub const FEATURE_BATCH: u64 = 1 << 6;
pub const FEATURE_PIPELINE: u64 = 1 << 7;
pub const FEATURE_BINARY_COPY: u64 = 1 << 8;
pub const FEATURE_SAVEPOINTS: u64 = 1 << 9;
pub const FEATURE_2PC: u64 = 1 << 10;
pub const FEATURE_CHECKSUMS: u64 = 1 << 11;

pub const QUERY_FLAG_DESCRIBE_ONLY: u32 = 0x01;
pub const QUERY_FLAG_NO_PORTAL: u32 = 0x02;
pub const QUERY_FLAG_BINARY_RESULT: u32 = 0x04;
pub const QUERY_FLAG_INCLUDE_PLAN: u32 = 0x08;
pub const QUERY_FLAG_RETURN_SBLR: u32 = 0x10;
pub const QUERY_FLAG_NO_CACHE: u32 = 0x20;

pub const ISOLATION_READ_UNCOMMITTED: u8 = 0;
pub const ISOLATION_READ_COMMITTED: u8 = 1;
pub const ISOLATION_REPEATABLE_READ: u8 = 2;
pub const ISOLATION_SERIALIZABLE: u8 = 3;

pub const READ_COMMITTED_MODE_DEFAULT: u8 = 0;
pub const READ_COMMITTED_MODE_READ_CONSISTENCY: u8 = 1;
pub const READ_COMMITTED_MODE_RECORD_VERSION: u8 = 2;
pub const READ_COMMITTED_MODE_NO_RECORD_VERSION: u8 = 3;

pub const TXN_FLAG_HAS_ISOLATION: u16 = 0x0001;
pub const TXN_FLAG_HAS_ACCESS: u16 = 0x0002;
pub const TXN_FLAG_HAS_DEFERRABLE: u16 = 0x0004;
pub const TXN_FLAG_HAS_WAIT: u16 = 0x0008;
pub const TXN_FLAG_HAS_TIMEOUT: u16 = 0x0010;
pub const TXN_FLAG_HAS_AUTOCOMMIT: u16 = 0x0020;
pub const TXN_FLAG_HAS_READ_COMMITTED_MODE: u16 = 0x0100;

pub const STREAM_START: u8 = 0;
pub const STREAM_PAUSE: u8 = 1;
pub const STREAM_RESUME: u8 = 2;
pub const STREAM_CANCEL: u8 = 3;
pub const STREAM_ACK: u8 = 4;

pub const SUB_TYPE_CHANNEL: u8 = 0;
pub const SUB_TYPE_TABLE: u8 = 1;
pub const SUB_TYPE_QUERY: u8 = 2;
pub const SUB_TYPE_EVENT: u8 = 3;

pub const AUTH_PARAM_METHOD_ID: &str = "auth_method_id";
pub const AUTH_PARAM_METHOD_PAYLOAD: &str = "auth_method_payload";
pub const AUTH_PARAM_PAYLOAD_JSON: &str = "auth_payload_json";
pub const AUTH_PARAM_PAYLOAD_B64: &str = "auth_payload_b64";
pub const AUTH_PARAM_PROVIDER_PROFILE: &str = "auth_provider_profile";
pub const AUTH_PARAM_REQUIRED_METHODS: &str = "auth_required_methods";
pub const AUTH_PARAM_FORBIDDEN_METHODS: &str = "auth_forbidden_methods";
pub const AUTH_PARAM_REQUIRE_CHANNEL_BINDING: &str = "auth_require_channel_binding";
pub const AUTH_PARAM_WORKLOAD_IDENTITY_TOKEN: &str = "workload_identity_token";
pub const AUTH_PARAM_PROXY_PRINCIPAL_ASSERTION: &str = "proxy_principal_assertion";

pub const AUTH_OK: u8 = 0;
pub const AUTH_PASSWORD: u8 = 1;
pub const AUTH_MD5: u8 = 2;
pub const AUTH_SCRAM_SHA256: u8 = 3;
pub const AUTH_SCRAM_SHA512: u8 = 4;
pub const AUTH_TOKEN: u8 = 5;
pub const AUTH_PEER: u8 = 6;
pub const AUTH_REATTACH: u8 = 7;

/// Copy format constants
pub const COPY_FORMAT_TEXT: u8 = 0;
pub const COPY_FORMAT_BINARY: u8 = 1;

/// Copy operation types for Close message
pub const COPY_CLOSE_STATEMENT: u8 = 0;
pub const COPY_CLOSE_PORTAL: u8 = 1;

#[derive(Debug, Clone)]
pub struct MessageHeader {
    pub msg_type: u8,
    pub flags: u8,
    pub length: u32,
    pub sequence: u32,
    pub attachment_id: [u8; 16],
    pub txn_id: u64,
}

#[derive(Debug, Clone)]
pub struct Message {
    pub header: MessageHeader,
    pub payload: Vec<u8>,
}

#[derive(Debug, Clone)]
pub struct ColumnInfo {
    pub name: String,
    pub table_oid: u32,
    pub column_index: u16,
    pub type_oid: u32,
    pub type_size: i16,
    pub type_modifier: i32,
    pub format: u8,
    pub nullable: bool,
}

#[derive(Debug, Clone)]
pub struct ColumnValue {
    pub data: Option<Vec<u8>>,
}

#[derive(Debug, Clone)]
pub struct ParamValue {
    pub format: u16,
    pub data: Option<Vec<u8>>,
}

#[derive(Debug, Clone, Default)]
pub struct AuthPluginSelection {
    pub method_id: String,
    pub method_payload: String,
    pub payload_json: String,
    pub payload_b64: String,
    pub provider_profile: String,
    pub required_methods: String,
    pub forbidden_methods: String,
    pub require_channel_binding: bool,
    pub workload_identity_token: String,
    pub proxy_principal_assertion: String,
}

pub fn apply_auth_plugin_selection(
    params: &mut HashMap<String, String>,
    selection: &AuthPluginSelection,
) -> Result<()> {
    let method_id = selection.method_id.trim();
    if !method_id.is_empty() && !method_id.starts_with("scratchbird.auth.") {
        return Err(Error::new(
            ErrorKind::Auth,
            "invalid auth_method_id namespace",
        ));
    }
    if !method_id.is_empty() {
        params.insert(AUTH_PARAM_METHOD_ID.to_string(), method_id.to_string());
    }
    if !selection.method_payload.is_empty() {
        params.insert(
            AUTH_PARAM_METHOD_PAYLOAD.to_string(),
            selection.method_payload.clone(),
        );
    }
    if !selection.payload_json.is_empty() {
        params.insert(
            AUTH_PARAM_PAYLOAD_JSON.to_string(),
            selection.payload_json.clone(),
        );
    }
    if !selection.payload_b64.is_empty() {
        params.insert(
            AUTH_PARAM_PAYLOAD_B64.to_string(),
            selection.payload_b64.clone(),
        );
    }
    if !selection.provider_profile.is_empty() {
        params.insert(
            AUTH_PARAM_PROVIDER_PROFILE.to_string(),
            selection.provider_profile.clone(),
        );
    }
    if !selection.required_methods.is_empty() {
        params.insert(
            AUTH_PARAM_REQUIRED_METHODS.to_string(),
            selection.required_methods.clone(),
        );
    }
    if !selection.forbidden_methods.is_empty() {
        params.insert(
            AUTH_PARAM_FORBIDDEN_METHODS.to_string(),
            selection.forbidden_methods.clone(),
        );
    }
    if selection.require_channel_binding {
        params.insert(
            AUTH_PARAM_REQUIRE_CHANNEL_BINDING.to_string(),
            "1".to_string(),
        );
    }
    if !selection.workload_identity_token.is_empty() {
        params.insert(
            AUTH_PARAM_WORKLOAD_IDENTITY_TOKEN.to_string(),
            selection.workload_identity_token.clone(),
        );
    }
    if !selection.proxy_principal_assertion.is_empty() {
        params.insert(
            AUTH_PARAM_PROXY_PRINCIPAL_ASSERTION.to_string(),
            selection.proxy_principal_assertion.clone(),
        );
    }
    Ok(())
}

#[derive(Debug, Clone)]
pub struct Notification {
    pub process_id: u32,
    pub channel: String,
    pub payload: Vec<u8>,
    pub change_type: Option<char>,
    pub row_id: Option<u64>,
}

#[derive(Debug, Clone)]
pub struct QueryPlan {
    pub format: u32,
    pub planning_time_us: u64,
    pub estimated_rows: u64,
    pub estimated_cost: u64,
    pub plan: Vec<u8>,
}

#[derive(Debug, Clone)]
pub struct SblrCompiled {
    pub hash: u64,
    pub version: u32,
    pub bytecode: Vec<u8>,
}

/// Copy operation response information
#[derive(Debug, Clone)]
pub struct CopyInResponse {
    pub format: u8,
    pub window_bytes: u32,
}

/// Copy out response information
#[derive(Debug, Clone)]
pub struct CopyOutResponse {
    pub format: u8,
    pub column_count: u16,
    pub column_formats: Vec<u32>,
}

/// Copy both response for bidirectional copy
#[derive(Debug, Clone)]
pub struct CopyBothResponse {
    pub format: u8,
    pub window_bytes: u32,
}

/// Copy data chunk from server
#[derive(Debug, Clone)]
pub struct CopyData {
    pub data: Vec<u8>,
}

/// Copy failure information
#[derive(Debug, Clone)]
pub struct CopyFailInfo {
    pub error_message: String,
}

pub fn encode_message(header: &MessageHeader, payload: &[u8]) -> Vec<u8> {
    let mut out = vec![0u8; HEADER_SIZE + payload.len()];
    out[0..4].copy_from_slice(&MAGIC_BYTES);
    out[4] = VERSION_MAJOR;
    out[5] = VERSION_MINOR;
    out[6] = header.msg_type;
    out[7] = header.flags;
    out[8..12].copy_from_slice(&(payload.len() as u32).to_le_bytes());
    out[12..16].copy_from_slice(&header.sequence.to_le_bytes());
    out[16..32].copy_from_slice(&header.attachment_id);
    out[32..40].copy_from_slice(&header.txn_id.to_le_bytes());
    out[HEADER_SIZE..].copy_from_slice(payload);
    out
}

pub fn decode_header(data: &[u8]) -> Result<MessageHeader> {
    if data.len() != HEADER_SIZE {
        return Err(Error::new(ErrorKind::Connection, "invalid header length"));
    }
    if data[0..4] != MAGIC_BYTES {
        return Err(Error::new(ErrorKind::Connection, "invalid protocol magic"));
    }
    if data[4] != VERSION_MAJOR || data[5] != VERSION_MINOR {
        return Err(Error::new(
            ErrorKind::Connection,
            "unsupported protocol version",
        ));
    }
    let length = u32::from_le_bytes([data[8], data[9], data[10], data[11]]);
    if length > MAX_MESSAGE_SIZE {
        return Err(Error::new(ErrorKind::Connection, "payload too large"));
    }
    let mut attachment_id = [0u8; 16];
    attachment_id.copy_from_slice(&data[16..32]);
    let txn_id = u64::from_le_bytes(data[32..40].try_into().unwrap_or([0u8; 8]));
    Ok(MessageHeader {
        msg_type: data[6],
        flags: data[7],
        length,
        sequence: u32::from_le_bytes([data[12], data[13], data[14], data[15]]),
        attachment_id,
        txn_id,
    })
}

pub fn build_startup_payload(features: u64, params: &HashMap<String, String>) -> Vec<u8> {
    let param_bytes = build_param_list(params);
    let mut out = Vec::with_capacity(2 + 2 + 8 + param_bytes.len());
    out.push(VERSION_MAJOR);
    out.push(VERSION_MINOR);
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(&features.to_le_bytes());
    out.extend_from_slice(&param_bytes);
    out
}

fn build_param_list(params: &HashMap<String, String>) -> Vec<u8> {
    let mut out = Vec::new();
    for (key, value) in params.iter() {
        out.extend_from_slice(key.as_bytes());
        out.push(0);
        out.extend_from_slice(value.as_bytes());
        out.push(0);
    }
    out.push(0);
    out
}

pub fn parse_auth_request(payload: &[u8]) -> Result<(u8, Vec<u8>)> {
    if payload.len() < 4 {
        return Err(Error::new(ErrorKind::Auth, "auth request truncated"));
    }
    let method = payload[0];
    Ok((method, payload[4..].to_vec()))
}

pub fn parse_auth_continue(payload: &[u8]) -> Result<(u8, u8, Vec<u8>)> {
    if payload.len() < 8 {
        return Err(Error::new(ErrorKind::Auth, "auth continue truncated"));
    }
    let method = payload[0];
    let stage = payload[1];
    let len = u32::from_le_bytes([payload[4], payload[5], payload[6], payload[7]]) as usize;
    if 8 + len > payload.len() {
        return Err(Error::new(ErrorKind::Auth, "auth continue truncated"));
    }
    Ok((method, stage, payload[8..8 + len].to_vec()))
}

pub fn parse_auth_ok(payload: &[u8]) -> Result<([u8; 16], Vec<u8>)> {
    if payload.len() < 20 {
        return Err(Error::new(ErrorKind::Auth, "auth ok truncated"));
    }
    let mut session_id = [0u8; 16];
    session_id.copy_from_slice(&payload[0..16]);
    let len = u32::from_le_bytes([payload[16], payload[17], payload[18], payload[19]]) as usize;
    if 20 + len > payload.len() {
        return Err(Error::new(ErrorKind::Auth, "auth ok truncated"));
    }
    Ok((session_id, payload[20..20 + len].to_vec()))
}

pub fn build_query_payload(sql: &str, flags: u32, max_rows: u32, timeout_ms: u32) -> Vec<u8> {
    let mut out = Vec::with_capacity(12 + sql.len() + 1);
    out.extend_from_slice(&flags.to_le_bytes());
    out.extend_from_slice(&max_rows.to_le_bytes());
    out.extend_from_slice(&timeout_ms.to_le_bytes());
    out.extend_from_slice(sql.as_bytes());
    out.push(0);
    out
}

pub fn build_parse_payload(statement_name: &str, sql: &str, param_types: &[u32]) -> Vec<u8> {
    let name_bytes = statement_name.as_bytes();
    let sql_bytes = sql.as_bytes();
    let mut out = Vec::with_capacity(
        4 + name_bytes.len() + 4 + sql_bytes.len() + 2 + 2 + param_types.len() * 4,
    );
    out.extend_from_slice(&(name_bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(name_bytes);
    out.extend_from_slice(&(sql_bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(sql_bytes);
    out.extend_from_slice(&(param_types.len() as u16).to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
    for oid in param_types {
        out.extend_from_slice(&oid.to_le_bytes());
    }
    out
}

pub fn build_bind_payload(
    portal_name: &str,
    statement_name: &str,
    params: &[ParamValue],
    result_formats: &[u16],
) -> Vec<u8> {
    let portal_bytes = portal_name.as_bytes();
    let stmt_bytes = statement_name.as_bytes();
    let param_formats: Vec<u16> = params.iter().map(|p| p.format).collect();

    let mut len = 4 + portal_bytes.len() + 4 + stmt_bytes.len();
    len += 2 + param_formats.len() * 2;
    len += 2 + 2;
    for param in params {
        len += 4;
        if let Some(ref data) = param.data {
            len += data.len();
        }
    }
    len += 2 + result_formats.len() * 2;

    let mut out = Vec::with_capacity(len);
    out.extend_from_slice(&(portal_bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(portal_bytes);
    out.extend_from_slice(&(stmt_bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(stmt_bytes);
    out.extend_from_slice(&(param_formats.len() as u16).to_le_bytes());
    for fmt in param_formats {
        out.extend_from_slice(&fmt.to_le_bytes());
    }
    out.extend_from_slice(&(params.len() as u16).to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
    for param in params {
        match param.data {
            None => out.extend_from_slice(&(-1i32).to_le_bytes()),
            Some(ref data) => {
                out.extend_from_slice(&(data.len() as i32).to_le_bytes());
                out.extend_from_slice(data);
            }
        }
    }
    out.extend_from_slice(&(result_formats.len() as u16).to_le_bytes());
    for fmt in result_formats {
        out.extend_from_slice(&fmt.to_le_bytes());
    }
    out
}

pub fn build_execute_payload(portal_name: &str, max_rows: u32) -> Vec<u8> {
    let portal_bytes = portal_name.as_bytes();
    let mut out = Vec::with_capacity(4 + portal_bytes.len() + 4);
    out.extend_from_slice(&(portal_bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(portal_bytes);
    out.extend_from_slice(&max_rows.to_le_bytes());
    out
}

pub fn build_describe_payload(describe_type: u8, name: &str) -> Vec<u8> {
    let name_bytes = name.as_bytes();
    let mut out = Vec::with_capacity(8 + name_bytes.len());
    out.push(describe_type);
    out.extend_from_slice(&[0, 0, 0]);
    out.extend_from_slice(&(name_bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(name_bytes);
    out
}

pub fn build_cancel_payload(cancel_type: u32, target_sequence: u32) -> Vec<u8> {
    let mut out = Vec::with_capacity(8);
    out.extend_from_slice(&cancel_type.to_le_bytes());
    out.extend_from_slice(&target_sequence.to_le_bytes());
    out
}

pub fn build_close_payload(close_type: u8, name: &str) -> Vec<u8> {
    let name_bytes = name.as_bytes();
    let mut out = Vec::with_capacity(5 + name_bytes.len());
    out.push(close_type);
    out.extend_from_slice(&[0, 0, 0]);
    out.extend_from_slice(&(name_bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(name_bytes);
    out
}

pub fn build_sblr_execute_payload(
    sblr_hash: u64,
    sblr_bytecode: &[u8],
    params: &[ParamValue],
) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(&sblr_hash.to_le_bytes());
    out.extend_from_slice(&(sblr_bytecode.len() as u32).to_le_bytes());
    out.extend_from_slice(&(params.len() as u16).to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(sblr_bytecode);
    for param in params {
        match param.data {
            None => out.extend_from_slice(&(-1i32).to_le_bytes()),
            Some(ref data) => {
                out.extend_from_slice(&(data.len() as i32).to_le_bytes());
                out.extend_from_slice(data);
            }
        }
    }
    out
}

pub fn build_subscribe_payload(subscribe_type: u8, channel: &str, filter_expr: &str) -> Vec<u8> {
    let channel_bytes = channel.as_bytes();
    let filter_bytes = filter_expr.as_bytes();
    let mut out = Vec::with_capacity(8 + channel_bytes.len() + filter_bytes.len());
    out.push(subscribe_type);
    out.extend_from_slice(&[0, 0, 0]);
    out.extend_from_slice(&(channel_bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(channel_bytes);
    out.extend_from_slice(&(filter_bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(filter_bytes);
    out
}

pub fn build_unsubscribe_payload(channel: &str) -> Vec<u8> {
    let channel_bytes = channel.as_bytes();
    let mut out = Vec::with_capacity(4 + channel_bytes.len());
    out.extend_from_slice(&(channel_bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(channel_bytes);
    out
}

pub fn build_txn_begin_payload(
    flags: u16,
    conflict_action: u8,
    autocommit_mode: u8,
    isolation_level: u8,
    access_mode: u8,
    deferrable: u8,
    wait_mode: u8,
    timeout_ms: u32,
    read_committed_mode: u8,
) -> Vec<u8> {
    let mut out = Vec::with_capacity(if (flags & TXN_FLAG_HAS_READ_COMMITTED_MODE) != 0 {
        16
    } else {
        12
    });
    out.extend_from_slice(&flags.to_le_bytes());
    out.push(conflict_action);
    out.push(autocommit_mode);
    out.push(isolation_level);
    out.push(access_mode);
    out.push(deferrable);
    out.push(wait_mode);
    out.extend_from_slice(&timeout_ms.to_le_bytes());
    if (flags & TXN_FLAG_HAS_READ_COMMITTED_MODE) != 0 {
        out.push(read_committed_mode);
        out.extend_from_slice(&[0, 0, 0]);
    }
    out
}

pub fn canonical_read_committed_mode_label(mode: u8) -> String {
    match mode {
        READ_COMMITTED_MODE_DEFAULT => "READ COMMITTED".to_string(),
        READ_COMMITTED_MODE_READ_CONSISTENCY => "READ COMMITTED READ CONSISTENCY".to_string(),
        READ_COMMITTED_MODE_RECORD_VERSION => "READ COMMITTED RECORD VERSION".to_string(),
        READ_COMMITTED_MODE_NO_RECORD_VERSION => "READ COMMITTED NO RECORD VERSION".to_string(),
        _ => format!("UNKNOWN({mode})"),
    }
}

pub fn build_txn_commit_payload(flags: u8) -> Vec<u8> {
    vec![flags, 0, 0, 0]
}

pub fn build_txn_rollback_payload(flags: u8) -> Vec<u8> {
    vec![flags, 0, 0, 0]
}

pub fn build_txn_savepoint_payload(name: &str) -> Vec<u8> {
    let name_bytes = name.as_bytes();
    let mut out = Vec::with_capacity(4 + name_bytes.len());
    out.extend_from_slice(&(name_bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(name_bytes);
    out
}

pub fn build_txn_release_payload(name: &str) -> Vec<u8> {
    build_txn_savepoint_payload(name)
}

pub fn build_txn_rollback_to_payload(name: &str) -> Vec<u8> {
    build_txn_savepoint_payload(name)
}

pub fn build_set_option_payload(name: &str, value: &str) -> Vec<u8> {
    let name_bytes = name.as_bytes();
    let value_bytes = value.as_bytes();
    let mut out = Vec::with_capacity(8 + name_bytes.len() + value_bytes.len());
    out.extend_from_slice(&(name_bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(name_bytes);
    out.extend_from_slice(&(value_bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(value_bytes);
    out
}

pub fn build_stream_control_payload(
    control_type: u8,
    window_size: u32,
    timeout_ms: u32,
) -> Vec<u8> {
    let mut out = Vec::with_capacity(12);
    out.push(control_type);
    out.extend_from_slice(&[0, 0, 0]);
    out.extend_from_slice(&window_size.to_le_bytes());
    out.extend_from_slice(&timeout_ms.to_le_bytes());
    out
}

pub fn build_attach_create_payload(emulation_mode: &str, db_name: &str) -> Vec<u8> {
    let mode_bytes = emulation_mode.as_bytes();
    let db_bytes = db_name.as_bytes();
    let mut out = Vec::with_capacity(8 + mode_bytes.len() + db_bytes.len());
    out.extend_from_slice(&(mode_bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(mode_bytes);
    out.extend_from_slice(&(db_bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(db_bytes);
    out
}

// ============================================================================
// COPY Message Builders (SBWP 1.1)
// ============================================================================

/// Build a CopyData message payload
pub fn build_copy_data_payload(data: &[u8]) -> Vec<u8> {
    data.to_vec()
}

/// Build a CopyDone message payload (empty payload)
pub fn build_copy_done_payload() -> Vec<u8> {
    Vec::new()
}

/// Build a CopyFail message payload
pub fn build_copy_fail_payload(error_message: &str) -> Vec<u8> {
    let msg_bytes = error_message.as_bytes();
    let mut out = Vec::with_capacity(4 + msg_bytes.len());
    out.extend_from_slice(&(msg_bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(msg_bytes);
    out
}

/// Build CopyInResponse payload (server->client, but used for testing)
pub fn build_copy_in_response_payload(format: u8, window_bytes: u32) -> Vec<u8> {
    let mut out = Vec::with_capacity(5);
    out.push(format);
    out.extend_from_slice(&window_bytes.to_le_bytes());
    out
}

/// Build CopyOutResponse payload (server->client, but used for testing)
pub fn build_copy_out_response_payload(format: u8, column_formats: &[u32]) -> Vec<u8> {
    let mut out = Vec::with_capacity(5 + column_formats.len() * 4);
    out.push(format);
    out.extend_from_slice(&(column_formats.len() as u16).to_le_bytes());
    for fmt in column_formats {
        out.extend_from_slice(&fmt.to_le_bytes());
    }
    out
}

/// Build CopyBothResponse payload (server->client, but used for testing)
pub fn build_copy_both_response_payload(format: u8, window_bytes: u32) -> Vec<u8> {
    build_copy_in_response_payload(format, window_bytes)
}

// ============================================================================
// COPY Message Parsers (SBWP 1.1)
// ============================================================================

/// Parse a CopyInResponse message from the server
pub fn parse_copy_in_response(payload: &[u8]) -> Result<CopyInResponse> {
    if payload.len() < 5 {
        return Err(Error::new(
            ErrorKind::Connection,
            "copy in response truncated",
        ));
    }
    let format = payload[0];
    let window_bytes = u32::from_le_bytes(payload[1..5].try_into().unwrap_or([0u8; 4]));
    Ok(CopyInResponse {
        format,
        window_bytes,
    })
}

/// Parse a CopyOutResponse message from the server
pub fn parse_copy_out_response(payload: &[u8]) -> Result<CopyOutResponse> {
    if payload.len() < 3 {
        return Err(Error::new(
            ErrorKind::Connection,
            "copy out response truncated",
        ));
    }
    let format = payload[0];
    let column_count = u16::from_le_bytes([payload[1], payload[2]]);
    let mut offset = 3;
    let mut column_formats = Vec::with_capacity(column_count as usize);
    for _ in 0..column_count {
        if offset + 4 > payload.len() {
            return Err(Error::new(
                ErrorKind::Connection,
                "copy out response truncated",
            ));
        }
        column_formats.push(u32::from_le_bytes([
            payload[offset],
            payload[offset + 1],
            payload[offset + 2],
            payload[offset + 3],
        ]));
        offset += 4;
    }
    Ok(CopyOutResponse {
        format,
        column_count,
        column_formats,
    })
}

/// Parse a CopyBothResponse message from the server
pub fn parse_copy_both_response(payload: &[u8]) -> Result<CopyBothResponse> {
    let response = parse_copy_in_response(payload)?;
    Ok(CopyBothResponse {
        format: response.format,
        window_bytes: response.window_bytes,
    })
}

/// Parse a CopyData message from the server
pub fn parse_copy_data(payload: &[u8]) -> Result<CopyData> {
    Ok(CopyData {
        data: payload.to_vec(),
    })
}

/// Parse a CopyFail message from the server
pub fn parse_copy_fail(payload: &[u8]) -> Result<CopyFailInfo> {
    if payload.len() < 4 {
        return Err(Error::new(ErrorKind::Connection, "copy fail truncated"));
    }
    let msg_len = u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]) as usize;
    if 4 + msg_len > payload.len() {
        return Err(Error::new(ErrorKind::Connection, "copy fail truncated"));
    }
    let error_message = String::from_utf8_lossy(&payload[4..4 + msg_len]).to_string();
    Ok(CopyFailInfo { error_message })
}

pub fn parse_ready(payload: &[u8]) -> Result<(u8, u64, u64)> {
    if payload.len() < 20 {
        return Err(Error::new(ErrorKind::Connection, "ready truncated"));
    }
    let status = payload[0];
    let txn_id = u64::from_le_bytes(payload[4..12].try_into().unwrap_or([0u8; 8]));
    let visibility = u64::from_le_bytes(payload[12..20].try_into().unwrap_or([0u8; 8]));
    Ok((status, txn_id, visibility))
}

pub fn parse_txn_status(payload: &[u8]) -> Result<(u8, u64)> {
    if payload.len() < 12 {
        return Err(Error::new(
            ErrorKind::Connection,
            "txn status truncated",
        ));
    }
    let status = payload[0];
    let txn_id = u64::from_le_bytes(payload[4..12].try_into().unwrap_or([0u8; 8]));
    Ok((status, txn_id))
}

pub fn parse_parameter_status(payload: &[u8]) -> Result<(String, String)> {
    if payload.len() < 8 {
        return Err(Error::new(
            ErrorKind::Connection,
            "parameter status truncated",
        ));
    }
    let name_len = u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]) as usize;
    let name_start = 4;
    let name_end = name_start + name_len;
    if name_end + 4 > payload.len() {
        return Err(Error::new(
            ErrorKind::Connection,
            "parameter status truncated",
        ));
    }
    let name = String::from_utf8_lossy(&payload[name_start..name_end]).to_string();
    let value_len = u32::from_le_bytes([
        payload[name_end],
        payload[name_end + 1],
        payload[name_end + 2],
        payload[name_end + 3],
    ]) as usize;
    let value_start = name_end + 4;
    let value_end = value_start + value_len;
    if value_end > payload.len() {
        return Err(Error::new(
            ErrorKind::Connection,
            "parameter status truncated",
        ));
    }
    let value = String::from_utf8_lossy(&payload[value_start..value_end]).to_string();
    Ok((name, value))
}

pub fn parse_parameter_description(payload: &[u8]) -> Result<Vec<u32>> {
    if payload.len() < 4 {
        return Err(Error::new(
            ErrorKind::Connection,
            "parameter description truncated",
        ));
    }
    let count = u16::from_le_bytes([payload[0], payload[1]]) as usize;
    let mut offset = 4;
    let mut types = Vec::with_capacity(count);
    for _ in 0..count {
        if offset + 4 > payload.len() {
            return Err(Error::new(
                ErrorKind::Connection,
                "parameter description truncated",
            ));
        }
        types.push(u32::from_le_bytes([
            payload[offset],
            payload[offset + 1],
            payload[offset + 2],
            payload[offset + 3],
        ]));
        offset += 4;
    }
    Ok(types)
}

pub fn parse_row_description(payload: &[u8]) -> Result<Vec<ColumnInfo>> {
    if payload.len() < 4 {
        return Err(Error::new(
            ErrorKind::Connection,
            "row description truncated",
        ));
    }
    let count = u16::from_le_bytes([payload[0], payload[1]]) as usize;
    let mut offset = 4;
    let mut columns = Vec::with_capacity(count);
    for _ in 0..count {
        if offset + 4 > payload.len() {
            return Err(Error::new(
                ErrorKind::Connection,
                "row description truncated",
            ));
        }
        let name_len = u32::from_le_bytes([
            payload[offset],
            payload[offset + 1],
            payload[offset + 2],
            payload[offset + 3],
        ]) as usize;
        offset += 4;
        if offset + name_len > payload.len() {
            return Err(Error::new(
                ErrorKind::Connection,
                "row description truncated",
            ));
        }
        let name = String::from_utf8_lossy(&payload[offset..offset + name_len]).to_string();
        offset += name_len;
        if offset + 18 > payload.len() {
            return Err(Error::new(
                ErrorKind::Connection,
                "row description truncated",
            ));
        }
        let table_oid = u32::from_le_bytes([
            payload[offset],
            payload[offset + 1],
            payload[offset + 2],
            payload[offset + 3],
        ]);
        offset += 4;
        let column_index = u16::from_le_bytes([payload[offset], payload[offset + 1]]);
        offset += 2;
        let type_oid = u32::from_le_bytes([
            payload[offset],
            payload[offset + 1],
            payload[offset + 2],
            payload[offset + 3],
        ]);
        offset += 4;
        let type_size = i16::from_le_bytes([payload[offset], payload[offset + 1]]);
        offset += 2;
        let type_modifier = i32::from_le_bytes([
            payload[offset],
            payload[offset + 1],
            payload[offset + 2],
            payload[offset + 3],
        ]);
        offset += 4;
        let format = payload[offset];
        offset += 1;
        let nullable = payload[offset] == 1;
        offset += 1;
        offset += 2;
        columns.push(ColumnInfo {
            name,
            table_oid,
            column_index,
            type_oid,
            type_size,
            type_modifier,
            format,
            nullable,
        });
    }
    Ok(columns)
}

pub fn parse_data_row(payload: &[u8], column_count: usize) -> Result<Vec<ColumnValue>> {
    if payload.len() < 4 {
        return Err(Error::new(ErrorKind::Connection, "row data truncated"));
    }
    let count = u16::from_le_bytes([payload[0], payload[1]]) as usize;
    let null_bytes = u16::from_le_bytes([payload[2], payload[3]]) as usize;
    if count != column_count {
        return Err(Error::new(
            ErrorKind::Connection,
            "row data column count mismatch",
        ));
    }
    let mut offset = 4;
    if offset + null_bytes > payload.len() {
        return Err(Error::new(ErrorKind::Connection, "row data truncated"));
    }
    let null_bitmap = &payload[offset..offset + null_bytes];
    offset += null_bytes;
    let mut values = Vec::with_capacity(count);
    for idx in 0..count {
        let byte_index = idx / 8;
        let bit_index = idx % 8;
        let is_null =
            byte_index < null_bitmap.len() && (null_bitmap[byte_index] & (1 << bit_index)) != 0;
        if is_null {
            values.push(ColumnValue { data: None });
            continue;
        }
        if offset + 4 > payload.len() {
            return Err(Error::new(ErrorKind::Connection, "row data truncated"));
        }
        let len = i32::from_le_bytes([
            payload[offset],
            payload[offset + 1],
            payload[offset + 2],
            payload[offset + 3],
        ]);
        offset += 4;
        if len < 0 {
            values.push(ColumnValue { data: None });
            continue;
        }
        let len = len as usize;
        if offset + len > payload.len() {
            return Err(Error::new(ErrorKind::Connection, "row data truncated"));
        }
        values.push(ColumnValue {
            data: Some(payload[offset..offset + len].to_vec()),
        });
        offset += len;
    }
    Ok(values)
}

pub fn parse_command_complete(payload: &[u8]) -> Result<(u8, u64, u64, String)> {
    if payload.len() < 20 {
        return Err(Error::new(
            ErrorKind::Connection,
            "command complete truncated",
        ));
    }
    let command_type = payload[0];
    let rows = u64::from_le_bytes(payload[4..12].try_into().unwrap_or([0u8; 8]));
    let last_id = u64::from_le_bytes(payload[12..20].try_into().unwrap_or([0u8; 8]));
    let tag_bytes = &payload[20..];
    let null_pos = tag_bytes
        .iter()
        .position(|b| *b == 0)
        .unwrap_or(tag_bytes.len());
    let tag = String::from_utf8_lossy(&tag_bytes[..null_pos]).to_string();
    Ok((command_type, rows, last_id, tag))
}

pub fn parse_notification(payload: &[u8]) -> Result<Notification> {
    if payload.len() < 12 {
        return Err(Error::new(ErrorKind::Connection, "notification truncated"));
    }
    let mut offset = 0;
    let process_id = u32::from_le_bytes(payload[offset..offset + 4].try_into().unwrap_or([0u8; 4]));
    offset += 4;
    let channel_len =
        u32::from_le_bytes(payload[offset..offset + 4].try_into().unwrap_or([0u8; 4])) as usize;
    offset += 4;
    if offset + channel_len + 4 > payload.len() {
        return Err(Error::new(ErrorKind::Connection, "notification truncated"));
    }
    let channel = String::from_utf8_lossy(&payload[offset..offset + channel_len]).to_string();
    offset += channel_len;
    let data_len =
        u32::from_le_bytes(payload[offset..offset + 4].try_into().unwrap_or([0u8; 4])) as usize;
    offset += 4;
    if offset + data_len > payload.len() {
        return Err(Error::new(ErrorKind::Connection, "notification truncated"));
    }
    let payload_data = payload[offset..offset + data_len].to_vec();
    offset += data_len;
    let mut change_type = None;
    let mut row_id = None;
    if offset < payload.len() {
        change_type = Some(payload[offset] as char);
        offset += 1;
        if offset + 8 <= payload.len() {
            row_id = Some(u64::from_le_bytes(
                payload[offset..offset + 8].try_into().unwrap_or([0u8; 8]),
            ));
        }
    }
    Ok(Notification {
        process_id,
        channel,
        payload: payload_data,
        change_type,
        row_id,
    })
}

pub fn parse_query_plan(payload: &[u8]) -> Result<QueryPlan> {
    if payload.len() < 32 {
        return Err(Error::new(ErrorKind::Connection, "query plan truncated"));
    }
    let format = u32::from_le_bytes(payload[0..4].try_into().unwrap_or([0u8; 4]));
    let plan_len = u32::from_le_bytes(payload[4..8].try_into().unwrap_or([0u8; 4])) as usize;
    let planning_time_us = u64::from_le_bytes(payload[8..16].try_into().unwrap_or([0u8; 8]));
    let estimated_rows = u64::from_le_bytes(payload[16..24].try_into().unwrap_or([0u8; 8]));
    let estimated_cost = u64::from_le_bytes(payload[24..32].try_into().unwrap_or([0u8; 8]));
    if 32 + plan_len > payload.len() {
        return Err(Error::new(ErrorKind::Connection, "query plan truncated"));
    }
    let plan = payload[32..32 + plan_len].to_vec();
    Ok(QueryPlan {
        format,
        planning_time_us,
        estimated_rows,
        estimated_cost,
        plan,
    })
}

pub fn parse_sblr_compiled(payload: &[u8]) -> Result<SblrCompiled> {
    if payload.len() < 16 {
        return Err(Error::new(ErrorKind::Connection, "sblr compiled truncated"));
    }
    let hash = u64::from_le_bytes(payload[0..8].try_into().unwrap_or([0u8; 8]));
    let version = u32::from_le_bytes(payload[8..12].try_into().unwrap_or([0u8; 4]));
    let len = u32::from_le_bytes(payload[12..16].try_into().unwrap_or([0u8; 4])) as usize;
    if 16 + len > payload.len() {
        return Err(Error::new(ErrorKind::Connection, "sblr compiled truncated"));
    }
    Ok(SblrCompiled {
        hash,
        version,
        bytecode: payload[16..16 + len].to_vec(),
    })
}

pub fn parse_error_message(payload: &[u8]) -> Result<(String, String, String, String, String)> {
    let mut offset = 0;
    let mut severity = String::new();
    let mut sqlstate = String::new();
    let mut message = String::new();
    let mut detail = String::new();
    let mut hint = String::new();

    while offset < payload.len() {
        let field = payload[offset];
        offset += 1;
        if field == 0 {
            break;
        }
        let start = offset;
        while offset < payload.len() && payload[offset] != 0 {
            offset += 1;
        }
        if offset >= payload.len() {
            break;
        }
        let value = String::from_utf8_lossy(&payload[start..offset]).to_string();
        offset += 1;
        match field as char {
            'S' => severity = value,
            'C' => sqlstate = value,
            'M' => message = value,
            'D' => detail = value,
            'H' => hint = value,
            _ => {}
        }
    }

    Ok((severity, sqlstate, message, detail, hint))
}

#[cfg(test)]
mod tests {
    use super::{
        build_txn_begin_payload, canonical_read_committed_mode_label,
        READ_COMMITTED_MODE_READ_CONSISTENCY, TXN_FLAG_HAS_ISOLATION,
        TXN_FLAG_HAS_READ_COMMITTED_MODE, TXN_FLAG_HAS_TIMEOUT,
    };

    #[test]
    fn build_txn_begin_payload_expands_for_read_committed_mode() {
        let flags =
            TXN_FLAG_HAS_ISOLATION | TXN_FLAG_HAS_TIMEOUT | TXN_FLAG_HAS_READ_COMMITTED_MODE;
        let payload = build_txn_begin_payload(
            flags,
            0,
            0,
            1,
            0,
            0,
            0,
            25,
            READ_COMMITTED_MODE_READ_CONSISTENCY,
        );
        assert_eq!(payload.len(), 16);
        assert_eq!(u16::from_le_bytes([payload[0], payload[1]]), flags);
        assert_eq!(payload[4], 1);
        assert_eq!(u32::from_le_bytes(payload[8..12].try_into().unwrap()), 25);
        assert_eq!(payload[12], READ_COMMITTED_MODE_READ_CONSISTENCY);
    }

    #[test]
    fn canonical_read_committed_mode_label_documents_public_selector() {
        assert_eq!(
            canonical_read_committed_mode_label(READ_COMMITTED_MODE_READ_CONSISTENCY),
            "READ COMMITTED READ CONSISTENCY"
        );
        assert_eq!(canonical_read_committed_mode_label(99), "UNKNOWN(99)");
    }
}
