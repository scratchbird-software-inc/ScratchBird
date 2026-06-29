// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

use std::collections::{HashMap, HashSet};
use std::sync::Arc;
use std::time::Duration;

use base64::engine::general_purpose::STANDARD as BASE64_STANDARD;
use base64::Engine as _;
use rand::RngCore;
use serde_json::{json, Value as JsonValue};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::net::TcpStream;
#[cfg(unix)]
use tokio::net::UnixStream;
use tokio::time::timeout;
use tokio_rustls::client::TlsStream;
use tokio_rustls::TlsConnector;

use crate::circuit_breaker::{CircuitBreaker, CircuitBreakerConfig};
use crate::config::Config;
use crate::errors::{error_from_sqlstate, Error, ErrorKind, Result};
use crate::keepalive::{KeepaliveConfig, KeepaliveTracker};
use crate::metadata::{
    build_metadata_schema_tree, list_metadata_schema_paths, normalize_metadata_collection_name,
    resolve_metadata_collection_query, MetadataRow, MetadataSchemaTree, MetadataSchemaTreeNode,
    MetadataSchemaTreeOptions,
};
use crate::protocol;
use crate::protocol::MessageHeader;
use crate::scram::ScramExchange;
use crate::sql::{normalize, normalize_callable, split_top_level_statements, Params};
use crate::telemetry::{SpanContext, TelemetryCollector, TelemetryConfig};
use crate::types::{decode_value, encode_param, Column, Param, Value, FORMAT_BINARY};

const QUERY_FLAG_BINARY_RESULT: u32 = 0x04;
const MANAGER_PROTOCOL_MAGIC: u32 = 0x4244_4253; // SBDB
const MANAGER_PROTOCOL_VERSION: u16 = 0x0101;
const MANAGER_HEADER_SIZE: usize = 12;
const MANAGER_MAX_PAYLOAD_SIZE: u32 = 16 * 1024 * 1024;
const MCP_PROTOCOL_VERSION: u16 = 0x0100;

const MCP_MSG_CONNECT_RESPONSE: u8 = 0x02;
const MCP_MSG_AUTH_CHALLENGE: u8 = 0x12;
const MCP_MSG_AUTH_RESPONSE: u8 = 0x11;
const MCP_MSG_STATUS_RESPONSE: u8 = 0x64;
const MCP_MSG_HELLO: u8 = 0x65;
const MCP_MSG_AUTH_START: u8 = 0x66;
const MCP_MSG_AUTH_CONTINUE: u8 = 0x67;
const MCP_MSG_DB_CONNECT: u8 = 0x69;
const MCP_AUTH_METHOD_TOKEN: u8 = 4;

fn normalize_native_protocol(value: &str) -> Option<&'static str> {
    match value.trim().to_ascii_lowercase().as_str() {
        "" | "native" | "scratchbird" | "scratchbird-native" | "scratchbird_native" | "sbwp"
        | "postgres" | "postgresql" | "pg" | "jdbc" | "odbc" | "sql" | "mysql" | "mariadb"
        | "sqlite" | "duckdb" | "firebird" => Some("native"),
        _ => None,
    }
}

fn normalize_front_door_mode(value: &str) -> Option<&'static str> {
    match value.trim().to_ascii_lowercase().as_str() {
        "" | "direct" => Some("direct"),
        "manager_proxy" | "manager-proxy" | "managed" => Some("manager_proxy"),
        _ => None,
    }
}

fn normalize_ssl_mode(value: &str) -> Option<&'static str> {
    match value.trim().to_ascii_lowercase().as_str() {
        "" | "require" | "on" | "true" | "1" | "yes" | "allow" | "prefer" => Some("require"),
        "verify-ca" | "verifyca" => Some("verify-ca"),
        "verify-full" | "verifyfull" => Some("verify-full"),
        "disable" | "off" | "false" | "0" | "no" => Some("disable"),
        _ => None,
    }
}

fn normalize_compression_mode(value: &str) -> Option<&'static str> {
    match value.trim().to_ascii_lowercase().as_str() {
        "" | "off" | "none" | "false" | "0" | "no" => Some("off"),
        "zstd" | "on" | "true" | "1" | "yes" => Some("zstd"),
        _ => None,
    }
}

fn append_u16(out: &mut Vec<u8>, value: u16) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn append_u32(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn append_lpreface(out: &mut Vec<u8>, value: &str) {
    append_u32(out, value.len() as u32);
    out.extend_from_slice(value.as_bytes());
}

fn auth_method_name(method: u8) -> &'static str {
    match method {
        protocol::AUTH_OK => "OK",
        protocol::AUTH_PASSWORD => "PASSWORD",
        protocol::AUTH_MD5 => "MD5",
        protocol::AUTH_SCRAM_SHA256 => "SCRAM_SHA_256",
        protocol::AUTH_SCRAM_SHA512 => "SCRAM_SHA_512",
        protocol::AUTH_TOKEN => "TOKEN",
        protocol::AUTH_PEER => "PEER",
        protocol::AUTH_REATTACH => "REATTACH",
        _ => "",
    }
}

fn auth_plugin_id_for_method(method: u8, configured_method_id: &str) -> String {
    if !configured_method_id.trim().is_empty() {
        return configured_method_id.trim().to_string();
    }
    match method {
        protocol::AUTH_PASSWORD => "scratchbird.auth.password_compat",
        protocol::AUTH_MD5 => "scratchbird.auth.md5_legacy",
        protocol::AUTH_SCRAM_SHA256 => "scratchbird.auth.scram_sha_256",
        protocol::AUTH_SCRAM_SHA512 => "scratchbird.auth.scram_sha_512",
        protocol::AUTH_TOKEN => "scratchbird.auth.authkey_token",
        protocol::AUTH_PEER => "scratchbird.auth.peer_uid",
        protocol::AUTH_REATTACH => "scratchbird.auth.reattach",
        _ => "",
    }
    .to_string()
}

fn auth_method_executable_locally(method: u8) -> bool {
    matches!(
        method,
        protocol::AUTH_PASSWORD
            | protocol::AUTH_SCRAM_SHA256
            | protocol::AUTH_SCRAM_SHA512
            | protocol::AUTH_TOKEN
    )
}

fn auth_method_broker_required(method: u8) -> bool {
    method == protocol::AUTH_PEER
}

fn additional_continuation_possible(method: u8) -> bool {
    matches!(
        method,
        protocol::AUTH_SCRAM_SHA256
            | protocol::AUTH_SCRAM_SHA512
            | protocol::AUTH_TOKEN
            | protocol::AUTH_PEER
    )
}

fn describe_auth_method(method: u8, configured_method_id: &str) -> Option<AuthMethodSurface> {
    let method_name = auth_method_name(method);
    if method_name.is_empty() {
        return None;
    }
    Some(AuthMethodSurface {
        method_code: method,
        method_name: method_name.to_string(),
        plugin_id: auth_plugin_id_for_method(method, configured_method_id),
        executable_locally: auth_method_executable_locally(method),
        broker_required: auth_method_broker_required(method),
    })
}

pub struct Client {
    config: Config,
    stream: Option<Box<dyn AsyncReadWrite>>,
    connected: bool,
    autocommit: bool,
    attachment_id: [u8; 16],
    txn_id: u64,
    runtime_txn_active: bool,
    explicit_transaction: bool,
    sequence: u32,
    last_query_sequence: u32,
    authed: bool,
    parameters: HashMap<String, String>,
    notification_handlers: Vec<Box<dyn Fn(&protocol::Notification) + Send + Sync>>,
    last_plan: Option<protocol::QueryPlan>,
    last_sblr: Option<protocol::SblrCompiled>,
    portal_resume_pending: bool,
    circuit_breaker: Arc<CircuitBreaker>,
    telemetry: Arc<TelemetryCollector>,
    keepalive_tracker: Arc<KeepaliveTracker>,
    resolved_auth_context: ResolvedAuthContext,
}

#[derive(Debug, Clone)]
pub struct QueryResult {
    pub columns: Vec<Column>,
    pub rows: Vec<Vec<Value>>,
    pub row_count: i64,
    pub command_tag: String,
}

#[derive(Debug, Clone)]
pub struct FieldSummary {
    pub name: String,
    pub type_oid: u32,
    pub format: u16,
    pub nullable: bool,
}

#[derive(Debug, Clone)]
pub struct ResultSetSummary {
    pub rows: Vec<Vec<Value>>,
    pub row_count: i64,
    pub fields: Vec<FieldSummary>,
    pub command: String,
    pub last_insert_id: i64,
}

#[derive(Debug, Clone)]
struct SplitStatement {
    sql: String,
    params: Vec<Param>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AuthMethodSurface {
    pub method_code: u8,
    pub method_name: String,
    pub plugin_id: String,
    pub executable_locally: bool,
    pub broker_required: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct AuthProbeResult {
    pub reachable: bool,
    pub front_door_mode: String,
    pub resolved_host: String,
    pub resolved_port: u16,
    pub admitted_methods: Vec<AuthMethodSurface>,
    pub required_method_code: u8,
    pub required_method: String,
    pub required_plugin_method_id: String,
    pub required_method_broker_required: bool,
    pub additional_continuation_possible: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ResolvedAuthContext {
    pub front_door_mode: String,
    pub resolved_auth_method: String,
    pub resolved_auth_plugin_id: String,
    pub manager_authenticated: bool,
    pub attached: bool,
}

#[derive(Debug, Clone)]
pub struct BatchItemSummary {
    pub index: usize,
    pub row_count: i64,
    pub fields: Vec<FieldSummary>,
    pub command: String,
    pub last_insert_id: i64,
}

#[derive(Debug, Clone, Default)]
pub struct BatchSummary {
    pub items: Vec<BatchItemSummary>,
    pub total_row_count: i64,
}

pub struct QueryStream<'a> {
    client: &'a mut Client,
    columns: Vec<protocol::ColumnInfo>,
    row_count: i64,
    command_tag: String,
    done: bool,
    page_size: u32,
    telemetry: Arc<TelemetryCollector>,
    circuit_breaker: Arc<CircuitBreaker>,
    span: Option<SpanContext>,
    finalized: bool,
}

/// Represents the state of a copy operation
#[derive(Debug, Clone)]
pub enum CopyState {
    /// Waiting for the server to respond
    Waiting,
    /// Copy data can be sent to the server (COPY FROM)
    Sending { format: u8, window_bytes: u32 },
    /// Copy data is being received from the server (COPY TO)
    Receiving {
        format: u8,
        column_formats: Vec<u32>,
    },
    /// Bidirectional copy is active
    Both { format: u8, window_bytes: u32 },
    /// Copy operation completed successfully
    Complete,
    /// Copy operation failed
    Failed { error: String },
}

/// Options for copy operations
#[derive(Debug, Clone)]
pub struct CopyOptions {
    /// Format of the copy data (text or binary)
    pub format: u8,
    /// Maximum number of bytes to buffer before sending
    pub buffer_size: usize,
}

impl Default for CopyOptions {
    fn default() -> Self {
        Self {
            format: protocol::COPY_FORMAT_TEXT,
            buffer_size: 65536,
        }
    }
}

/// Result of a copy operation
#[derive(Debug, Clone)]
pub struct CopyResult {
    /// Number of rows affected
    pub rows_affected: u64,
    /// Command tag from the server
    pub command_tag: String,
}

#[derive(Debug, Clone, Default)]
pub struct TxnBeginOptions {
    /// Public isolation aliases map onto the canonical MGA modes:
    /// READ COMMITTED => READ COMMITTED
    /// REPEATABLE READ => SNAPSHOT
    /// SERIALIZABLE => SNAPSHOT TABLE STABILITY
    /// read_committed_mode selects the canonical READ COMMITTED sub-mode.
    pub isolation_level: Option<u8>,
    pub read_committed_mode: Option<u8>,
    pub access_mode: Option<u8>,
    pub deferrable: Option<bool>,
    pub wait: Option<bool>,
    pub timeout_ms: Option<u32>,
    pub autocommit_mode: Option<u8>,
    pub conflict_action: u8,
}

#[derive(Debug, Clone, Default)]
pub struct TxnEndOptions {
    pub flags: u8,
}

trait AsyncReadWrite: AsyncRead + AsyncWrite + Unpin + Send {}
impl<T> AsyncReadWrite for T where T: AsyncRead + AsyncWrite + Unpin + Send {}

fn encode_txn_begin_options(opts: &TxnBeginOptions) -> Result<Vec<u8>> {
    let mut flags = 0u16;
    let mut isolation = opts
        .isolation_level
        .unwrap_or(protocol::ISOLATION_READ_COMMITTED);
    if opts.isolation_level.is_some() {
        flags |= protocol::TXN_FLAG_HAS_ISOLATION;
    }
    let read_committed_mode = opts
        .read_committed_mode
        .unwrap_or(protocol::READ_COMMITTED_MODE_DEFAULT);
    if opts.read_committed_mode.is_some() {
        if let Some(explicit_isolation) = opts.isolation_level {
            if explicit_isolation != protocol::ISOLATION_READ_UNCOMMITTED
                && explicit_isolation != protocol::ISOLATION_READ_COMMITTED
            {
                return Err(Error::with_sqlstate(
                    ErrorKind::NotSupported,
                    "read_committed_mode requires a READ COMMITTED isolation alias",
                    Some("0A000".to_string()),
                    None,
                    None,
                ));
            }
        } else {
            isolation = protocol::ISOLATION_READ_COMMITTED;
            flags |= protocol::TXN_FLAG_HAS_ISOLATION;
        }
        flags |= protocol::TXN_FLAG_HAS_READ_COMMITTED_MODE;
    }
    if opts.access_mode.is_some() {
        flags |= protocol::TXN_FLAG_HAS_ACCESS;
    }
    if opts.deferrable.is_some() {
        flags |= protocol::TXN_FLAG_HAS_DEFERRABLE;
    }
    if opts.wait.is_some() {
        flags |= protocol::TXN_FLAG_HAS_WAIT;
    }
    if opts.timeout_ms.is_some() {
        flags |= protocol::TXN_FLAG_HAS_TIMEOUT;
    }
    if opts.autocommit_mode.is_some() {
        flags |= protocol::TXN_FLAG_HAS_AUTOCOMMIT;
    }
    Ok(protocol::build_txn_begin_payload(
        flags,
        opts.conflict_action,
        opts.autocommit_mode.unwrap_or(0),
        isolation,
        opts.access_mode.unwrap_or(0),
        if opts.deferrable.unwrap_or(false) {
            1
        } else {
            0
        },
        if opts.wait.unwrap_or(false) { 1 } else { 0 },
        opts.timeout_ms.unwrap_or(0),
        read_committed_mode,
    ))
}

impl Client {
    pub fn new(config: Config) -> Self {
        Self {
            config,
            stream: None,
            connected: false,
            autocommit: true,
            attachment_id: [0u8; 16],
            txn_id: 0,
            runtime_txn_active: false,
            explicit_transaction: false,
            sequence: 0,
            last_query_sequence: 0,
            authed: false,
            parameters: HashMap::new(),
            notification_handlers: Vec::new(),
            last_plan: None,
            last_sblr: None,
            portal_resume_pending: false,
            circuit_breaker: Arc::new(CircuitBreaker::new(CircuitBreakerConfig::default())),
            telemetry: Arc::new(TelemetryCollector::new(TelemetryConfig::default())),
            keepalive_tracker: Arc::new(KeepaliveTracker::new(KeepaliveConfig::default())),
            resolved_auth_context: ResolvedAuthContext::default(),
        }
    }

    pub async fn connect(&mut self) -> Result<()> {
        self.reset_resolved_auth_context();
        let startup_params = self.preflight_connect()?;
        self.close().await;
        let manager_proxy = self.config.front_door_mode == "manager_proxy";
        let stream = self.connect_transport().await?;
        self.stream = Some(stream);
        let connect_result = async {
            if manager_proxy {
                self.perform_manager_connect().await?;
            }
            self.handshake(startup_params).await?;
            self.apply_schema().await?;
            Ok(())
        }
        .await;
        if let Err(err) = connect_result {
            self.close().await;
            return Err(err);
        }
        self.connected = true;
        Ok(())
    }

    pub fn get_resolved_auth_context(&self) -> ResolvedAuthContext {
        self.resolved_auth_context.clone()
    }

    pub async fn probe_auth_surface(&mut self) -> Result<AuthProbeResult> {
        self.reset_resolved_auth_context();
        self.normalize_connection_config(false, false)?;
        self.close().await;
        let stream = self.connect_transport().await?;
        self.stream = Some(stream);
        let resolved_host = self.config.host.clone();
        let resolved_port = self.config.port;
        let result = if self.config.front_door_mode == "manager_proxy" {
            self.probe_manager_auth_surface(resolved_host, resolved_port)
                .await
        } else {
            let params = self.build_startup_params()?;
            self.probe_direct_auth_surface(params, resolved_host, resolved_port)
                .await
        };
        self.close().await;
        result
    }

    fn reset_resolved_auth_context(&mut self) {
        self.resolved_auth_context = ResolvedAuthContext {
            front_door_mode: if self.config.front_door_mode.trim().is_empty() {
                "direct".to_string()
            } else {
                self.config.front_door_mode.clone()
            },
            resolved_auth_method: String::new(),
            resolved_auth_plugin_id: String::new(),
            manager_authenticated: false,
            attached: false,
        };
    }

    fn mark_resolved_auth_context_detached(&mut self) {
        self.resolved_auth_context.attached = false;
        self.resolved_auth_context.manager_authenticated = false;
    }

    fn preflight_connect(&mut self) -> Result<HashMap<String, String>> {
        self.normalize_connection_config(true, true)?;
        self.build_startup_params()
    }

    fn normalize_connection_config(
        &mut self,
        require_identity: bool,
        require_manager_token: bool,
    ) -> Result<()> {
        let protocol = normalize_native_protocol(&self.config.protocol).ok_or_else(|| {
            Error::with_sqlstate(
                ErrorKind::NotSupported,
                "only protocol=native is supported; connect to the native parser listener/port",
                Some("0A000".to_string()),
                None,
                None,
            )
        })?;
        self.config.protocol = protocol.to_string();

        let front_door_mode =
            normalize_front_door_mode(&self.config.front_door_mode).ok_or_else(|| {
                Error::with_sqlstate(
                    ErrorKind::NotSupported,
                    "front_door_mode must be direct or manager_proxy",
                    Some("0A000".to_string()),
                    None,
                    None,
                )
            })?;
        self.config.front_door_mode = front_door_mode.to_string();

        let ssl_mode = normalize_ssl_mode(&self.config.sslmode).ok_or_else(|| {
            Error::with_sqlstate(
                ErrorKind::NotSupported,
                "sslmode must be disable, require, verify-ca, or verify-full",
                Some("0A000".to_string()),
                None,
                None,
            )
        })?;
        self.config.sslmode = ssl_mode.to_string();

        let compression_mode =
            normalize_compression_mode(&self.config.compression).ok_or_else(|| {
                Error::with_sqlstate(
                    ErrorKind::NotSupported,
                    "compression must be off or zstd",
                    Some("0A000".to_string()),
                    None,
                    None,
                )
            })?;
        self.config.compression = compression_mode.to_string();

        if require_identity && (self.config.user.is_empty() || self.config.database.is_empty()) {
            return Err(Error::new(
                ErrorKind::Connection,
                "user and database are required",
            ));
        }
        if require_manager_token
            && self.config.front_door_mode == "manager_proxy"
            && self.config.manager_auth_token.is_empty()
        {
            return Err(Error::new(
                ErrorKind::Connection,
                "manager_proxy mode requires manager_auth_token",
            ));
        }
        Ok(())
    }

    fn build_startup_params(&self) -> Result<HashMap<String, String>> {
        let mut params = HashMap::new();
        params.insert("database".to_string(), self.config.database.clone());
        params.insert("user".to_string(), self.config.user.clone());
        params.insert(
            "client_flags".to_string(),
            self.config.connect_client_flags.to_string(),
        );
        if !self.config.role.is_empty() {
            params.insert("role".to_string(), self.config.role.clone());
        }
        if !self.config.application_name.is_empty() {
            params.insert(
                "application_name".to_string(),
                self.config.application_name.clone(),
            );
        }
        let method_id = if !self.config.auth_method_id.is_empty() {
            self.config.auth_method_id.clone()
        } else {
            self.config
                .extra
                .get(protocol::AUTH_PARAM_METHOD_ID)
                .cloned()
                .unwrap_or_default()
        };
        let method_payload = if !self.config.auth_method_payload.is_empty() {
            self.config.auth_method_payload.clone()
        } else {
            self.config
                .extra
                .get(protocol::AUTH_PARAM_METHOD_PAYLOAD)
                .cloned()
                .unwrap_or_default()
        };
        let payload_json = if !self.config.auth_payload_json.is_empty() {
            self.config.auth_payload_json.clone()
        } else {
            self.config
                .extra
                .get(protocol::AUTH_PARAM_PAYLOAD_JSON)
                .cloned()
                .unwrap_or_default()
        };
        let payload_b64 = if !self.config.auth_payload_b64.is_empty() {
            self.config.auth_payload_b64.clone()
        } else {
            self.config
                .extra
                .get(protocol::AUTH_PARAM_PAYLOAD_B64)
                .cloned()
                .unwrap_or_default()
        };
        let provider_profile = if !self.config.auth_provider_profile.is_empty() {
            self.config.auth_provider_profile.clone()
        } else {
            self.config
                .extra
                .get(protocol::AUTH_PARAM_PROVIDER_PROFILE)
                .cloned()
                .unwrap_or_default()
        };
        let required_methods = if !self.config.auth_required_methods.is_empty() {
            self.config.auth_required_methods.clone()
        } else {
            self.config
                .extra
                .get(protocol::AUTH_PARAM_REQUIRED_METHODS)
                .cloned()
                .unwrap_or_default()
        };
        let forbidden_methods = if !self.config.auth_forbidden_methods.is_empty() {
            self.config.auth_forbidden_methods.clone()
        } else {
            self.config
                .extra
                .get(protocol::AUTH_PARAM_FORBIDDEN_METHODS)
                .cloned()
                .unwrap_or_default()
        };
        let require_channel_binding = if self.config.auth_require_channel_binding {
            true
        } else {
            self.config
                .extra
                .get(protocol::AUTH_PARAM_REQUIRE_CHANNEL_BINDING)
                .map(|value| {
                    matches!(
                        value.trim().to_ascii_lowercase().as_str(),
                        "1" | "true" | "yes" | "on"
                    )
                })
                .unwrap_or(false)
        };
        let workload_identity_token = if !self.config.workload_identity_token.is_empty() {
            self.config.workload_identity_token.clone()
        } else {
            self.config
                .extra
                .get(protocol::AUTH_PARAM_WORKLOAD_IDENTITY_TOKEN)
                .cloned()
                .unwrap_or_default()
        };
        let proxy_principal_assertion = if !self.config.proxy_principal_assertion.is_empty() {
            self.config.proxy_principal_assertion.clone()
        } else {
            self.config
                .extra
                .get(protocol::AUTH_PARAM_PROXY_PRINCIPAL_ASSERTION)
                .cloned()
                .unwrap_or_default()
        };
        let auth_plugin_selection = protocol::AuthPluginSelection {
            method_id,
            method_payload,
            payload_json,
            payload_b64,
            provider_profile,
            required_methods,
            forbidden_methods,
            require_channel_binding,
            workload_identity_token,
            proxy_principal_assertion,
        };
        protocol::apply_auth_plugin_selection(&mut params, &auth_plugin_selection)?;
        Ok(params)
    }

    pub async fn close(&mut self) {
        if let Some(mut stream) = self.stream.take() {
            let _ = stream.shutdown().await;
        }
        self.clear_abandoned_session_state();
    }

    pub async fn query(&mut self, sql: &str) -> Result<QueryResult> {
        self.query_params(sql, Params::Positional(Vec::new())).await
    }

    pub fn native_sql(&self, sql: &str, params: Params) -> Result<String> {
        let normalized = normalize(sql, params)?;
        Ok(normalized.sql)
    }

    pub fn native_callable_sql(&self, sql: &str, params: Params) -> Result<String> {
        let normalized = normalize_callable(sql, params)?;
        Ok(normalized.sql)
    }

    pub async fn query_params(&mut self, sql: &str, params: Params) -> Result<QueryResult> {
        self.ensure_connected()?;
        let span = self.begin_operation("query").await?;
        let normalized = normalize(sql, params)?;
        if normalized.params.is_empty() {
            self.send_simple_query(&normalized.sql, 0, 0).await?;
        } else {
            self.send_extended_query(&normalized.sql, &normalized.params, 0)
                .await?;
        }
        let result = self.collect_results().await;
        self.end_operation(span, result.is_ok()).await;
        result
    }

    pub async fn call(&mut self, sql: &str, params: Params) -> Result<QueryResult> {
        self.ensure_connected()?;
        let span = self.begin_operation("call").await?;
        let normalized = normalize_callable(sql, params)?;
        if normalized.params.is_empty() {
            self.send_simple_query(&normalized.sql, 0, 0).await?;
        } else {
            self.send_extended_query(&normalized.sql, &normalized.params, 0)
                .await?;
        }
        let result = self.collect_results().await;
        self.end_operation(span, result.is_ok()).await;
        result
    }

    pub async fn query_multi(
        &mut self,
        sql: &str,
        params: Params,
    ) -> Result<Vec<ResultSetSummary>> {
        self.ensure_connected()?;
        let span = self.begin_operation("query_multi").await?;
        let normalized = normalize(sql, params)?;
        if let Some(statements) =
            self.split_executable_statements(&normalized.sql, &normalized.params)?
        {
            let mut all_sets = Vec::new();
            for statement in statements {
                if statement.params.is_empty() {
                    self.send_simple_query(&statement.sql, 0, 0).await?;
                } else {
                    self.send_extended_query(&statement.sql, &statement.params, 0)
                        .await?;
                }
                let mut sets = self.collect_result_sets().await?;
                all_sets.append(&mut sets);
            }
            self.end_operation(span, true).await;
            return Ok(all_sets);
        }
        if normalized.params.is_empty() {
            self.send_simple_query(&normalized.sql, 0, 0).await?;
        } else {
            self.send_extended_query(&normalized.sql, &normalized.params, 0)
                .await?;
        }
        let result = self.collect_result_sets().await;
        self.end_operation(span, result.is_ok()).await;
        result
    }

    pub async fn execute_multi(
        &mut self,
        sql: &str,
        params: Params,
    ) -> Result<Vec<ResultSetSummary>> {
        self.query_multi(sql, params).await
    }

    fn split_executable_statements(
        &self,
        sql: &str,
        params: &[Param],
    ) -> Result<Option<Vec<SplitStatement>>> {
        let statements = split_top_level_statements(sql);
        if statements.len() <= 1 {
            return Ok(None);
        }
        let mut split = Vec::with_capacity(statements.len());
        for statement in statements {
            split.push(self.remap_statement_params(&statement, params)?);
        }
        Ok(Some(split))
    }

    fn remap_statement_params(&self, sql: &str, params: &[Param]) -> Result<SplitStatement> {
        if params.is_empty() {
            return Ok(SplitStatement {
                sql: sql.to_string(),
                params: Vec::new(),
            });
        }

        let chars: Vec<char> = sql.chars().collect();
        let mut result = String::new();
        let mut in_single = false;
        let mut in_double = false;
        let mut remap: HashMap<usize, usize> = HashMap::new();
        let mut ordered_indexes = Vec::new();
        let mut i = 0;
        while i < chars.len() {
            let ch = chars[i];
            if ch == '\'' && !in_double {
                in_single = !in_single;
                result.push(ch);
                i += 1;
                continue;
            }
            if ch == '"' && !in_single {
                in_double = !in_double;
                result.push(ch);
                i += 1;
                continue;
            }
            if !in_single
                && !in_double
                && ch == '$'
                && i + 1 < chars.len()
                && chars[i + 1].is_ascii_digit()
            {
                let mut j = i + 1;
                while j < chars.len() && chars[j].is_ascii_digit() {
                    j += 1;
                }
                let original_index: usize = chars[i + 1..j]
                    .iter()
                    .collect::<String>()
                    .parse()
                    .map_err(|_| {
                        Error::with_sqlstate(
                            ErrorKind::Syntax,
                            "parameter count mismatch",
                            Some("07001".to_string()),
                            None,
                            None,
                        )
                    })?;
                let next_index = if let Some(existing) = remap.get(&original_index) {
                    *existing
                } else {
                    let next = ordered_indexes.len() + 1;
                    remap.insert(original_index, next);
                    ordered_indexes.push(original_index);
                    next
                };
                result.push('$');
                result.push_str(&next_index.to_string());
                i = j;
                continue;
            }
            result.push(ch);
            i += 1;
        }

        let mut remapped_params = Vec::with_capacity(ordered_indexes.len());
        for original_index in ordered_indexes {
            if original_index == 0 || original_index > params.len() {
                return Err(Error::with_sqlstate(
                    ErrorKind::Syntax,
                    "parameter count mismatch",
                    Some("07001".to_string()),
                    None,
                    None,
                ));
            }
            remapped_params.push(params[original_index - 1].clone());
        }

        Ok(SplitStatement {
            sql: result,
            params: remapped_params,
        })
    }

    pub async fn execute_batch(
        &mut self,
        sql: &str,
        batch_params: Vec<Params>,
    ) -> Result<BatchSummary> {
        if batch_params.is_empty() {
            return Err(Error::with_sqlstate(
                ErrorKind::Syntax,
                "batch arguments are required",
                Some("07001".to_string()),
                None,
                None,
            ));
        }
        let mut summary = BatchSummary::default();
        for (index, params) in batch_params.into_iter().enumerate() {
            let result_sets = self.query_multi(sql, params).await?;
            let mut row_count = 0_i64;
            let mut fields = Vec::new();
            let mut command = String::new();
            let mut last_insert_id = 0_i64;

            for set in &result_sets {
                if set.row_count > 0 {
                    row_count += set.row_count;
                }
                if !set.fields.is_empty() {
                    fields = set.fields.clone();
                }
                if !set.command.is_empty() {
                    command = set.command.clone();
                }
                if set.last_insert_id != 0 {
                    last_insert_id = set.last_insert_id;
                }
            }
            if row_count > 0 {
                summary.total_row_count += row_count;
            }
            summary.items.push(BatchItemSummary {
                index,
                row_count,
                fields,
                command,
                last_insert_id,
            });
        }
        Ok(summary)
    }

    pub async fn query_batch(
        &mut self,
        sql: &str,
        batch_params: Vec<Params>,
    ) -> Result<BatchSummary> {
        self.execute_batch(sql, batch_params).await
    }

    pub async fn execute_with_generated_keys(
        &mut self,
        sql: &str,
        params: Params,
    ) -> Result<Vec<i64>> {
        let sets = self.query_multi(sql, params).await?;
        let mut keys = Vec::with_capacity(sets.len());
        for set in sets {
            if set.last_insert_id != 0 {
                keys.push(set.last_insert_id);
            }
        }
        Ok(keys)
    }

    pub async fn query_stream(&mut self, sql: &str) -> Result<QueryStream<'_>> {
        self.ensure_connected()?;
        let span = self.begin_operation("query_stream").await?;
        let telemetry = Arc::clone(&self.telemetry);
        let circuit_breaker = Arc::clone(&self.circuit_breaker);
        let page_size = self.config.fetch_size.max(1);
        self.send_simple_query(sql, page_size, 0).await?;
        Ok(QueryStream {
            client: self,
            columns: Vec::new(),
            row_count: -1,
            command_tag: String::new(),
            done: false,
            page_size,
            telemetry,
            circuit_breaker,
            span,
            finalized: false,
        })
    }

    pub async fn query_stream_params(
        &mut self,
        sql: &str,
        params: Params,
    ) -> Result<QueryStream<'_>> {
        self.ensure_connected()?;
        let span = self.begin_operation("query_stream").await?;
        let telemetry = Arc::clone(&self.telemetry);
        let circuit_breaker = Arc::clone(&self.circuit_breaker);
        let normalized = normalize(sql, params)?;
        let page_size = self.config.fetch_size.max(1);
        if normalized.params.is_empty() {
            self.send_simple_query(&normalized.sql, page_size, 0)
                .await?;
        } else {
            self.send_extended_query(&normalized.sql, &normalized.params, page_size)
                .await?;
        }
        Ok(QueryStream {
            client: self,
            columns: Vec::new(),
            row_count: -1,
            command_tag: String::new(),
            done: false,
            page_size,
            telemetry,
            circuit_breaker,
            span,
            finalized: false,
        })
    }

    pub async fn query_metadata(&mut self, collection: &str) -> Result<QueryResult> {
        let Some(query) = resolve_metadata_collection_query(collection) else {
            return Err(Error::with_sqlstate(
                ErrorKind::NotSupported,
                format!("metadata collection '{}' is not supported", collection),
                Some("0A000".to_string()),
                None,
                None,
            ));
        };
        self.query(query).await
    }

    pub async fn query_metadata_with_restrictions(
        &mut self,
        collection: &str,
        restrictions: &HashMap<String, String>,
    ) -> Result<QueryResult> {
        let result = self.query_metadata(collection).await?;
        Ok(apply_metadata_restrictions(
            result,
            restrictions,
            Some(collection),
        ))
    }

    pub fn metadata_collection_name(collection: &str) -> Result<&'static str> {
        normalize_metadata_collection_name(collection).ok_or_else(|| {
            Error::with_sqlstate(
                ErrorKind::NotSupported,
                format!("metadata collection '{}' is not supported", collection),
                Some("0A000".to_string()),
                None,
                None,
            )
        })
    }

    pub async fn get_schema(
        &mut self,
        collection: &str,
        restrictions: Option<&HashMap<String, String>>,
    ) -> Result<Vec<MetadataRow>> {
        let result = if let Some(restrictions) = restrictions {
            self.query_metadata_with_restrictions(collection, restrictions)
                .await?
        } else {
            self.query_metadata(collection).await?
        };
        Ok(query_result_to_metadata_rows(&result))
    }

    pub async fn get_schema_tree(
        &mut self,
        schema_pattern: Option<&str>,
        expand_parents: Option<bool>,
    ) -> Result<MetadataSchemaTree> {
        let rows = self.get_schema("schemas", None).await?;
        let rows = filter_schema_metadata_rows_by_pattern(rows, schema_pattern);
        let expand = expand_parents.unwrap_or(self.config.metadata_expand_schema_parents);

        Ok(build_metadata_schema_tree(
            &rows,
            MetadataSchemaTreeOptions {
                expand_parents: expand,
                database: if self.config.database.is_empty() {
                    None
                } else {
                    Some(self.config.database.clone())
                },
            },
        ))
    }

    pub async fn ddl_editor_schema_payload(
        &mut self,
        schema_pattern: Option<&str>,
        expand_schema_parents: Option<bool>,
    ) -> Result<JsonValue> {
        let rows = self.get_schema("schemas", None).await?;
        let rows = filter_schema_metadata_rows_by_pattern(rows, schema_pattern);
        let expand = expand_schema_parents.unwrap_or(self.config.metadata_expand_schema_parents);
        let schema_paths = list_metadata_schema_paths(&rows, expand);
        let tree = build_metadata_schema_tree(
            &rows,
            MetadataSchemaTreeOptions {
                expand_parents: expand,
                database: None,
            },
        );

        Ok(json!({
            "schemaPattern": schema_pattern,
            "expandSchemaParents": expand,
            "schemaPaths": schema_paths,
            "schemaTree": schema_tree_nodes_to_payload(&tree.schemas),
        }))
    }

    pub fn autocommit(&self) -> bool {
        self.autocommit
    }

    pub async fn set_autocommit(&mut self, enabled: bool) -> Result<()> {
        self.ensure_connected()?;
        if self.autocommit == enabled {
            return Ok(());
        }
        if enabled && self.has_active_transaction() {
            self.commit_transaction(None).await?;
        }
        // Native engine endpoints own the replacement transaction boundary.
        // Autocommit transitions are local driver policy, not SET_OPTION wire
        // state or synthetic begin calls.
        self.autocommit = enabled;
        Ok(())
    }

    pub async fn begin(&mut self, options: Option<TxnBeginOptions>) -> Result<()> {
        self.begin_transaction(options).await
    }

    pub async fn commit(&mut self, options: Option<TxnEndOptions>) -> Result<()> {
        self.commit_transaction(options).await
    }

    pub async fn rollback(&mut self, options: Option<TxnEndOptions>) -> Result<()> {
        self.rollback_transaction(options).await
    }

    pub fn supports_prepared_transactions(&self) -> bool {
        true
    }

    pub fn supports_dormant_reattach(&self) -> bool {
        false
    }

    pub async fn prepare_transaction(&mut self, global_transaction_id: &str) -> Result<()> {
        let sql =
            Self::build_prepared_transaction_sql("PREPARE TRANSACTION", global_transaction_id)?;
        self.query(&sql).await?;
        Ok(())
    }

    pub async fn commit_prepared(&mut self, global_transaction_id: &str) -> Result<()> {
        let sql = Self::build_prepared_transaction_sql("COMMIT PREPARED", global_transaction_id)?;
        self.query(&sql).await?;
        Ok(())
    }

    pub async fn rollback_prepared(&mut self, global_transaction_id: &str) -> Result<()> {
        let sql = Self::build_prepared_transaction_sql("ROLLBACK PREPARED", global_transaction_id)?;
        self.query(&sql).await?;
        Ok(())
    }

    pub async fn detach_to_dormant(&mut self) -> Result<()> {
        Err(Error::with_sqlstate(
            ErrorKind::NotSupported,
            "dormant detach is not supported by the Rust driver",
            Some("0A000".to_string()),
            None,
            None,
        ))
    }

    pub async fn reattach_dormant(
        &mut self,
        _dormant_id: &str,
        _auth_token: Option<&str>,
    ) -> Result<()> {
        Err(Error::with_sqlstate(
            ErrorKind::NotSupported,
            "dormant reattach is not supported by the Rust driver",
            Some("0A000".to_string()),
            None,
            None,
        ))
    }

    pub async fn begin_transaction(&mut self, options: Option<TxnBeginOptions>) -> Result<()> {
        self.ensure_connected()?;
        let opts = options.unwrap_or_default();
        if self.explicit_transaction {
            return Err(Self::invalid_txn_state("transaction already active"));
        }
        let payload = encode_txn_begin_options(&opts)?;
        self.send_message(protocol::MSG_TXN_BEGIN, &payload, 0, false)
            .await?;
        self.drain_until_ready().await?;
        self.explicit_transaction = true;
        Ok(())
    }

    pub async fn commit_transaction(&mut self, options: Option<TxnEndOptions>) -> Result<()> {
        self.ensure_connected()?;
        self.ensure_transaction_active("commit")?;
        let flags = options.map(|opt| opt.flags).unwrap_or(0);
        let payload = protocol::build_txn_commit_payload(flags);
        self.send_message(protocol::MSG_TXN_COMMIT, &payload, 0, false)
            .await?;
        self.drain_until_ready().await?;
        self.explicit_transaction = false;
        self.drain_immediate_reopen_boundary().await
    }

    pub async fn rollback_transaction(&mut self, options: Option<TxnEndOptions>) -> Result<()> {
        self.ensure_connected()?;
        self.ensure_transaction_active("rollback")?;
        let flags = options.map(|opt| opt.flags).unwrap_or(0);
        let payload = protocol::build_txn_rollback_payload(flags);
        self.send_message(protocol::MSG_TXN_ROLLBACK, &payload, 0, false)
            .await?;
        self.drain_until_ready().await?;
        self.explicit_transaction = false;
        self.drain_immediate_reopen_boundary().await
    }

    pub async fn savepoint(&mut self, name: &str) -> Result<()> {
        self.ensure_connected()?;
        self.ensure_transaction_active("savepoint")?;
        let name = Self::validate_savepoint_name(name)?;
        let payload = protocol::build_txn_savepoint_payload(name);
        self.send_message(protocol::MSG_TXN_SAVEPOINT, &payload, 0, false)
            .await?;
        self.drain_until_ready().await
    }

    pub async fn release_savepoint(&mut self, name: &str) -> Result<()> {
        self.ensure_connected()?;
        self.ensure_transaction_active("release savepoint")?;
        let name = Self::validate_savepoint_name(name)?;
        let payload = protocol::build_txn_release_payload(name);
        self.send_message(protocol::MSG_TXN_RELEASE, &payload, 0, false)
            .await?;
        self.drain_until_ready().await
    }

    pub async fn rollback_to_savepoint(&mut self, name: &str) -> Result<()> {
        self.ensure_connected()?;
        self.ensure_transaction_active("rollback to savepoint")?;
        let name = Self::validate_savepoint_name(name)?;
        let payload = protocol::build_txn_rollback_to_payload(name);
        self.send_message(protocol::MSG_TXN_ROLLBACK_TO, &payload, 0, false)
            .await?;
        self.drain_until_ready().await
    }

    pub async fn set_option(&mut self, name: &str, value: &str) -> Result<()> {
        self.ensure_connected()?;
        let payload = protocol::build_set_option_payload(name, value);
        self.send_message(protocol::MSG_SET_OPTION, &payload, 0, false)
            .await?;
        self.drain_until_ready().await
    }

    pub async fn ping(&mut self) -> Result<()> {
        self.ensure_connected()?;
        self.send_message(protocol::MSG_PING, &[], 0, false).await?;
        loop {
            let msg = self.recv_message().await?;
            if self.handle_async_message(&msg)? {
                continue;
            }
            if msg.header.msg_type == protocol::MSG_PONG
                || msg.header.msg_type == protocol::MSG_READY
            {
                if msg.header.msg_type == protocol::MSG_READY {
                    let (status, txn_id, _visibility) = protocol::parse_ready(&msg.payload)?;
                    self.apply_runtime_ready_state(status, txn_id);
                    self.portal_resume_pending = false;
                }
                return Ok(());
            }
            if msg.header.msg_type == protocol::MSG_ERROR {
                return self.raise_protocol_error(&msg.payload);
            }
        }
    }

    pub async fn terminate(&mut self) -> Result<()> {
        if !self.connected {
            self.close().await;
            return Ok(());
        }
        self.send_message(protocol::MSG_TERMINATE, &[], 0, false)
            .await?;
        self.close().await;
        Ok(())
    }

    pub async fn subscribe(
        &mut self,
        subscribe_type: u8,
        channel: &str,
        filter_expr: &str,
    ) -> Result<()> {
        self.ensure_connected()?;
        let payload = protocol::build_subscribe_payload(subscribe_type, channel, filter_expr);
        self.send_message(protocol::MSG_SUBSCRIBE, &payload, 0, false)
            .await?;
        self.drain_until_ready().await
    }

    pub async fn unsubscribe(&mut self, channel: &str) -> Result<()> {
        self.ensure_connected()?;
        let payload = protocol::build_unsubscribe_payload(channel);
        self.send_message(protocol::MSG_UNSUBSCRIBE, &payload, 0, false)
            .await?;
        self.drain_until_ready().await
    }

    pub async fn execute_sblr(
        &mut self,
        sblr_hash: u64,
        sblr_bytecode: &[u8],
        params: &[Param],
    ) -> Result<QueryResult> {
        self.ensure_connected()?;
        let span = self.begin_operation("sblr_execute").await?;
        let mut encoded = Vec::with_capacity(params.len());
        for param in params {
            let (value, _oid) = encode_param(param)?;
            encoded.push(value);
        }
        let payload = protocol::build_sblr_execute_payload(sblr_hash, sblr_bytecode, &encoded);
        self.last_plan = None;
        self.last_sblr = None;
        self.portal_resume_pending = false;
        let sequence = self
            .send_message(protocol::MSG_SBLR_EXECUTE, &payload, 0, false)
            .await?;
        self.last_query_sequence = sequence;
        self.send_message(protocol::MSG_SYNC, &[], 0, false).await?;
        let result = self.collect_results().await;
        self.end_operation(span, result.is_ok()).await;
        result
    }

    pub async fn compile_sblr(&mut self, sql: &str) -> Result<protocol::SblrCompiled> {
        self.ensure_connected()?;
        let span = self.begin_operation("compile_sblr").await?;
        self.last_sblr = None;
        self.send_simple_query_with_flags(sql, 0, 0, protocol::QUERY_FLAG_RETURN_SBLR)
            .await?;
        let drained = self.drain_until_ready().await;
        if let Err(err) = drained {
            self.end_operation(span, false).await;
            return Err(err);
        }
        let Some(compiled) = self.last_sblr.clone() else {
            self.end_operation(span, false).await;
            return Err(Error::with_sqlstate(
                ErrorKind::Connection,
                "parser endpoint did not return SBLR for RETURN_SBLR request",
                Some("08P01".to_string()),
                None,
                None,
            ));
        };
        self.end_operation(span, true).await;
        Ok(compiled)
    }

    pub async fn stream_control(
        &mut self,
        control_type: u8,
        window_size: u32,
        timeout_ms: u32,
    ) -> Result<()> {
        self.ensure_connected()?;
        let payload = protocol::build_stream_control_payload(control_type, window_size, timeout_ms);
        self.send_message(protocol::MSG_STREAM_CONTROL, &payload, 0, false)
            .await?;
        Ok(())
    }

    pub async fn attach_create(&mut self, emulation_mode: &str, db_name: &str) -> Result<()> {
        self.ensure_connected()?;
        let payload = protocol::build_attach_create_payload(emulation_mode, db_name);
        self.send_message(protocol::MSG_ATTACH_CREATE, &payload, 0, false)
            .await?;
        self.drain_until_ready().await
    }

    pub async fn attach_detach(&mut self) -> Result<()> {
        self.ensure_connected()?;
        self.send_message(protocol::MSG_ATTACH_DETACH, &[], 0, false)
            .await?;
        self.drain_until_ready().await
    }

    pub async fn attach_list(&mut self) -> Result<QueryResult> {
        self.ensure_connected()?;
        self.send_message(protocol::MSG_ATTACH_LIST, &[], 0, false)
            .await?;
        self.send_message(protocol::MSG_SYNC, &[], 0, false).await?;
        self.collect_results().await
    }

    pub fn on_notification<F>(&mut self, handler: F)
    where
        F: Fn(&protocol::Notification) + Send + Sync + 'static,
    {
        self.notification_handlers.push(Box::new(handler));
    }

    pub fn last_query_plan(&self) -> Option<&protocol::QueryPlan> {
        self.last_plan.as_ref()
    }

    pub fn last_sblr_compiled(&self) -> Option<&protocol::SblrCompiled> {
        self.last_sblr.as_ref()
    }

    fn clear_abandoned_session_state(&mut self) {
        // MGA reconnect creates a fresh attachment/transaction boundary. Session
        // metadata captured from the abandoned attachment must not survive into
        // the replacement handshake.
        self.connected = false;
        self.authed = false;
        self.attachment_id = [0u8; 16];
        self.clear_transaction_state();
        self.sequence = 0;
        self.last_query_sequence = 0;
        self.parameters.clear();
        self.last_plan = None;
        self.last_sblr = None;
        self.portal_resume_pending = false;
        self.mark_resolved_auth_context_detached();
    }

    // ============================================================================
    // COPY Operations (SBWP 1.1)
    // ============================================================================

    /// Execute a COPY FROM operation (client sends data to server).
    ///
    /// # Arguments
    /// * `sql` - The COPY SQL statement (e.g., "COPY table FROM STDIN")
    /// * `data` - The data to be copied
    /// * `options` - Optional copy options
    ///
    /// # Example
    /// ```ignore
    /// let data = b"1,hello\n2,world\n".to_vec();
    /// let result = client.copy_in("COPY my_table FROM STDIN (FORMAT csv)", data, None).await?;
    /// ```
    pub async fn copy_in(
        &mut self,
        sql: &str,
        data: Vec<u8>,
        options: Option<CopyOptions>,
    ) -> Result<CopyResult> {
        self.ensure_connected()?;
        let span = self.begin_operation("copy_in").await?;
        let opts = options.unwrap_or_default();

        // Request binary copy feature if using binary format
        if opts.format == protocol::COPY_FORMAT_BINARY {
            self.request_binary_copy_feature().await?;
        }

        // Send the COPY query
        self.send_simple_query(sql, 0, 0).await?;

        // Wait for CopyInResponse
        let copy_response = self.wait_for_copy_in_response().await?;

        let result = match copy_response {
            CopyState::Sending {
                format: _,
                window_bytes: _,
            } => {
                // Send the copy data in chunks
                self.send_copy_data_in_chunks(&data, opts.buffer_size)
                    .await?;

                // Send CopyDone
                self.send_copy_done().await?;

                // Wait for CommandComplete
                self.wait_for_copy_complete().await
            }
            CopyState::Failed { error } => Err(Error::new(
                ErrorKind::Data,
                format!("copy failed: {}", error),
            )),
            _ => Err(Error::new(
                ErrorKind::Connection,
                "unexpected copy response",
            )),
        };
        self.end_operation(span, result.is_ok()).await;
        result
    }

    /// Execute a COPY TO operation (server sends data to client).
    ///
    /// # Arguments
    /// * `sql` - The COPY SQL statement (e.g., "COPY table TO STDOUT")
    /// * `options` - Optional copy options
    ///
    /// # Returns
    /// The data received from the server
    pub async fn copy_out(&mut self, sql: &str, options: Option<CopyOptions>) -> Result<Vec<u8>> {
        self.ensure_connected()?;
        let span = self.begin_operation("copy_out").await?;
        let opts = options.unwrap_or_default();

        // Request binary copy feature if using binary format
        if opts.format == protocol::COPY_FORMAT_BINARY {
            self.request_binary_copy_feature().await?;
        }

        // Send the COPY query
        self.send_simple_query(sql, 0, 0).await?;

        // Wait for CopyOutResponse and collect data
        let result = self.collect_copy_out_data().await;
        self.end_operation(span, result.is_ok()).await;
        result
    }

    /// Send copy data in chunks to avoid memory issues with large datasets.
    ///
    /// # Arguments
    /// * `sql` - The COPY SQL statement
    /// * `data_stream` - Async stream of data chunks
    pub async fn copy_in_streaming<F, Fut>(
        &mut self,
        sql: &str,
        mut data_stream: F,
    ) -> Result<CopyResult>
    where
        F: FnMut() -> Fut,
        Fut: std::future::Future<Output = Result<Option<Vec<u8>>>>,
    {
        self.ensure_connected()?;
        let span = self.begin_operation("copy_in_stream").await?;

        // Send the COPY query
        self.send_simple_query(sql, 0, 0).await?;

        // Wait for CopyInResponse
        let copy_response = self.wait_for_copy_in_response().await?;

        let result = match copy_response {
            CopyState::Sending { .. } => {
                // Stream the data
                loop {
                    match data_stream().await? {
                        Some(chunk) => {
                            let payload = protocol::build_copy_data_payload(&chunk);
                            self.send_message(protocol::MSG_COPY_DATA, &payload, 0, false)
                                .await?;
                        }
                        None => break,
                    }
                }

                // Send CopyDone
                self.send_copy_done().await?;

                // Wait for CommandComplete
                self.wait_for_copy_complete().await
            }
            CopyState::Failed { error } => Err(Error::new(
                ErrorKind::Data,
                format!("copy failed: {}", error),
            )),
            _ => Err(Error::new(
                ErrorKind::Connection,
                "unexpected copy response",
            )),
        };
        self.end_operation(span, result.is_ok()).await;
        result
    }

    /// Send a chunk of copy data.
    /// This is a low-level method for advanced use cases.
    pub async fn send_copy_data(&mut self, data: &[u8]) -> Result<()> {
        let payload = protocol::build_copy_data_payload(data);
        self.send_message(protocol::MSG_COPY_DATA, &payload, 0, false)
            .await?;
        Ok(())
    }

    /// Signal that all copy data has been sent.
    /// This is a low-level method for advanced use cases.
    pub async fn send_copy_done(&mut self) -> Result<()> {
        let payload = protocol::build_copy_done_payload();
        self.send_message(protocol::MSG_COPY_DONE, &payload, 0, false)
            .await?;
        Ok(())
    }

    /// Signal a copy failure with an error message.
    /// This is a low-level method for advanced use cases.
    pub async fn send_copy_fail(&mut self, error_message: &str) -> Result<()> {
        let payload = protocol::build_copy_fail_payload(error_message);
        self.send_message(protocol::MSG_COPY_FAIL, &payload, 0, false)
            .await?;
        Ok(())
    }

    // ============================================================================
    // Private COPY helper methods
    // ============================================================================

    async fn request_binary_copy_feature(&mut self) -> Result<()> {
        // The binary copy feature is negotiated during handshake
        // This method can be used to verify the feature is available
        Ok(())
    }

    async fn wait_for_copy_in_response(&mut self) -> Result<CopyState> {
        loop {
            let msg = self.recv_message().await?;
            if self.handle_async_message(&msg)? {
                continue;
            }
            match msg.header.msg_type {
                protocol::MSG_COPY_IN_RESPONSE => {
                    let response = protocol::parse_copy_in_response(&msg.payload)?;
                    return Ok(CopyState::Sending {
                        format: response.format,
                        window_bytes: response.window_bytes,
                    });
                }
                protocol::MSG_ERROR => {
                    let (_, _, message, _, _) = protocol::parse_error_message(&msg.payload)?;
                    return Ok(CopyState::Failed { error: message });
                }
                protocol::MSG_READY => {
                    let (status, txn_id, _visibility) = protocol::parse_ready(&msg.payload)?;
                    self.apply_runtime_ready_state(status, txn_id);
                    return Ok(CopyState::Complete);
                }
                _ => continue,
            }
        }
    }

    async fn send_copy_data_in_chunks(&mut self, data: &[u8], chunk_size: usize) -> Result<()> {
        for chunk in data.chunks(chunk_size) {
            let payload = protocol::build_copy_data_payload(chunk);
            self.send_message(protocol::MSG_COPY_DATA, &payload, 0, false)
                .await?;
        }
        Ok(())
    }

    async fn wait_for_copy_complete(&mut self) -> Result<CopyResult> {
        loop {
            let msg = self.recv_message().await?;
            if self.handle_async_message(&msg)? {
                continue;
            }
            match msg.header.msg_type {
                protocol::MSG_COMMAND_COMPLETE => {
                    let (_cmd_type, rows_affected, _last_id, tag) =
                        protocol::parse_command_complete(&msg.payload)?;
                    return Ok(CopyResult {
                        rows_affected,
                        command_tag: tag,
                    });
                }
                protocol::MSG_ERROR => return self.raise_protocol_error(&msg.payload),
                protocol::MSG_READY => {
                    let (status, txn_id, _visibility) = protocol::parse_ready(&msg.payload)?;
                    self.apply_runtime_ready_state(status, txn_id);
                    return Ok(CopyResult {
                        rows_affected: 0,
                        command_tag: "COPY".to_string(),
                    });
                }
                _ => continue,
            }
        }
    }

    async fn collect_copy_out_data(&mut self) -> Result<Vec<u8>> {
        let mut result = Vec::new();

        loop {
            let msg = self.recv_message().await?;
            if self.handle_async_message(&msg)? {
                continue;
            }
            match msg.header.msg_type {
                protocol::MSG_COPY_OUT_RESPONSE => {
                    let _response = protocol::parse_copy_out_response(&msg.payload)?;
                    // The response tells us the format, we just collect data
                    continue;
                }
                protocol::MSG_COPY_DATA => {
                    let data = protocol::parse_copy_data(&msg.payload)?;
                    result.extend_from_slice(&data.data);
                }
                protocol::MSG_COPY_DONE => {
                    // Copy operation complete, wait for CommandComplete
                    continue;
                }
                protocol::MSG_COMMAND_COMPLETE => {
                    // Copy operation fully complete
                    continue;
                }
                protocol::MSG_READY => {
                    let (status, txn_id, _visibility) = protocol::parse_ready(&msg.payload)?;
                    self.apply_runtime_ready_state(status, txn_id);
                    return Ok(result);
                }
                protocol::MSG_ERROR => return self.raise_protocol_error(&msg.payload),
                _ => continue,
            }
        }
    }

    pub async fn cancel(&mut self) -> Result<()> {
        let payload = protocol::build_cancel_payload(0, self.last_query_sequence);
        self.send_message(
            protocol::MSG_CANCEL,
            &payload,
            protocol::MSG_FLAG_URGENT,
            false,
        )
        .await
        .map(|_| ())
    }

    async fn send_manager_frame(&mut self, msg_type: u8, payload: &[u8]) -> Result<()> {
        let stream = self
            .stream
            .as_mut()
            .ok_or_else(|| Error::new(ErrorKind::Connection, "no active socket"))?;
        let mut frame = Vec::with_capacity(MANAGER_HEADER_SIZE + payload.len());
        append_u32(&mut frame, MANAGER_PROTOCOL_MAGIC);
        append_u16(&mut frame, MANAGER_PROTOCOL_VERSION);
        frame.push(msg_type);
        frame.push(0);
        append_u32(&mut frame, payload.len() as u32);
        frame.extend_from_slice(payload);
        if self.config.socket_timeout_ms > 0 {
            timeout(
                Duration::from_millis(self.config.socket_timeout_ms),
                stream.write_all(&frame),
            )
            .await
            .map_err(|_| Error::new(ErrorKind::Connection, "socket write timeout"))??;
        } else {
            stream.write_all(&frame).await?;
        }
        Ok(())
    }

    async fn recv_manager_frame(&mut self) -> Result<(u8, Vec<u8>)> {
        let mut header = [0u8; MANAGER_HEADER_SIZE];
        self.read_exact(&mut header).await?;
        let magic = u32::from_le_bytes([header[0], header[1], header[2], header[3]]);
        if magic != MANAGER_PROTOCOL_MAGIC {
            return Err(Error::new(
                ErrorKind::Connection,
                "manager frame magic mismatch",
            ));
        }
        let version = u16::from_le_bytes([header[4], header[5]]);
        if version != MANAGER_PROTOCOL_VERSION {
            return Err(Error::new(
                ErrorKind::Connection,
                "manager frame version mismatch",
            ));
        }
        let msg_type = header[6];
        let payload_len = u32::from_le_bytes([header[8], header[9], header[10], header[11]]);
        if payload_len > MANAGER_MAX_PAYLOAD_SIZE {
            return Err(Error::new(
                ErrorKind::Connection,
                "manager payload too large",
            ));
        }
        let mut payload = vec![0u8; payload_len as usize];
        if payload_len > 0 {
            self.read_exact(&mut payload).await?;
        }
        Ok((msg_type, payload))
    }

    async fn perform_manager_connect(&mut self) -> Result<()> {
        if self.config.manager_auth_token.is_empty() {
            return Err(Error::new(
                ErrorKind::Connection,
                "manager_proxy mode requires manager_auth_token",
            ));
        }
        let manager_user = if !self.config.manager_username.is_empty() {
            self.config.manager_username.clone()
        } else if !self.config.user.is_empty() {
            self.config.user.clone()
        } else {
            "admin".to_string()
        };
        let manager_database = if !self.config.manager_database.is_empty() {
            self.config.manager_database.clone()
        } else {
            self.config.database.clone()
        };
        let manager_profile = if !self.config.manager_connection_profile.is_empty() {
            self.config.manager_connection_profile.clone()
        } else {
            "SBsql".to_string()
        };
        let manager_intent = if !self.config.manager_client_intent.is_empty() {
            self.config.manager_client_intent.clone()
        } else {
            "SBsql".to_string()
        };

        let hello = {
            let mut out = Vec::with_capacity(4);
            append_u16(&mut out, MCP_PROTOCOL_VERSION);
            append_u16(&mut out, self.config.manager_client_flags);
            out
        };
        self.send_manager_frame(MCP_MSG_HELLO, &hello).await?;
        let (mut msg_type, mut payload) = self.recv_manager_frame().await?;
        if msg_type != MCP_MSG_STATUS_RESPONSE {
            return Err(Error::new(
                ErrorKind::Connection,
                "expected MCP hello status response",
            ));
        }

        let mut auth_start = Vec::new();
        append_lpreface(&mut auth_start, &manager_user);
        auth_start.push(MCP_AUTH_METHOD_TOKEN);
        if self.config.manager_auth_fast_path {
            append_u32(&mut auth_start, self.config.manager_auth_token.len() as u32);
            auth_start.extend_from_slice(self.config.manager_auth_token.as_bytes());
        } else {
            append_u32(&mut auth_start, 0);
        }
        self.send_manager_frame(MCP_MSG_AUTH_START, &auth_start)
            .await?;
        (msg_type, payload) = self.recv_manager_frame().await?;
        if msg_type == MCP_MSG_AUTH_CHALLENGE {
            let mut auth_continue = Vec::new();
            append_u32(
                &mut auth_continue,
                self.config.manager_auth_token.len() as u32,
            );
            auth_continue.extend_from_slice(self.config.manager_auth_token.as_bytes());
            self.send_manager_frame(MCP_MSG_AUTH_CONTINUE, &auth_continue)
                .await?;
            (msg_type, payload) = self.recv_manager_frame().await?;
        }
        if msg_type != MCP_MSG_AUTH_RESPONSE {
            return Err(Error::new(
                ErrorKind::Connection,
                "expected MCP auth response",
            ));
        }
        if payload.len() < 1 + 4 + 256 {
            return Err(Error::new(
                ErrorKind::Connection,
                "truncated MCP auth response",
            ));
        }
        if payload[0] != 0 {
            let mut error_bytes = payload[5..(5 + 256)].to_vec();
            if let Some(pos) = error_bytes.iter().position(|b| *b == 0) {
                error_bytes.truncate(pos);
            }
            let err_text = String::from_utf8_lossy(&error_bytes).to_string();
            return Err(Error::with_sqlstate(
                ErrorKind::Auth,
                if err_text.is_empty() {
                    "MCP authentication failed".to_string()
                } else {
                    err_text
                },
                Some("28000".to_string()),
                None,
                None,
            ));
        }
        self.resolved_auth_context.manager_authenticated = true;

        let mut db_connect = b"MCP1".to_vec();
        append_lpreface(&mut db_connect, &manager_database);
        append_lpreface(&mut db_connect, &manager_profile);
        append_lpreface(&mut db_connect, &manager_intent);
        let mut nonce = [0u8; 16];
        rand::thread_rng().fill_bytes(&mut nonce);
        append_u16(&mut db_connect, nonce.len() as u16);
        db_connect.extend_from_slice(&nonce);
        self.send_manager_frame(MCP_MSG_DB_CONNECT, &db_connect)
            .await?;
        let (msg_type, payload) = self.recv_manager_frame().await?;
        if msg_type != MCP_MSG_CONNECT_RESPONSE {
            return Err(Error::new(
                ErrorKind::Connection,
                "expected MCP connect response",
            ));
        }
        if payload.len() < 1 + 2 + 2 + 16 + 64 + 32 {
            return Err(Error::new(
                ErrorKind::Connection,
                "truncated MCP connect response",
            ));
        }
        if payload[0] != 0 {
            let mut err_text = "MCP database connect failed".to_string();
            let err_offset = 1 + 2 + 2 + 16 + 64 + 32;
            if payload.len() >= err_offset + 4 {
                let err_len = u32::from_le_bytes([
                    payload[err_offset],
                    payload[err_offset + 1],
                    payload[err_offset + 2],
                    payload[err_offset + 3],
                ]) as usize;
                if payload.len() >= err_offset + 4 + err_len {
                    err_text = String::from_utf8_lossy(
                        &payload[(err_offset + 4)..(err_offset + 4 + err_len)],
                    )
                    .to_string();
                }
            }
            return Err(Error::with_sqlstate(
                ErrorKind::Auth,
                err_text,
                Some("28000".to_string()),
                None,
                None,
            ));
        }
        Ok(())
    }

    async fn probe_direct_auth_surface(
        &mut self,
        params: HashMap<String, String>,
        resolved_host: String,
        resolved_port: u16,
    ) -> Result<AuthProbeResult> {
        let features = self.requested_features();
        let payload = protocol::build_startup_payload(features, &params);
        self.send_message(protocol::MSG_STARTUP, &payload, 0, true)
            .await?;

        loop {
            let msg = self.recv_message().await?;
            match msg.header.msg_type {
                protocol::MSG_NEGOTIATE_VERSION => continue,
                protocol::MSG_PARAMETER_STATUS => continue,
                protocol::MSG_AUTH_REQUEST => {
                    let (method, _) = protocol::parse_auth_request(&msg.payload)?;
                    let admitted_methods =
                        describe_auth_method(method, &self.config.auth_method_id)
                            .map(|surface| vec![surface])
                            .unwrap_or_default();
                    return Ok(AuthProbeResult {
                        reachable: true,
                        front_door_mode: "direct".to_string(),
                        resolved_host,
                        resolved_port,
                        admitted_methods,
                        required_method_code: method,
                        required_method: auth_method_name(method).to_string(),
                        required_plugin_method_id: auth_plugin_id_for_method(
                            method,
                            &self.config.auth_method_id,
                        ),
                        required_method_broker_required: auth_method_broker_required(method),
                        additional_continuation_possible: additional_continuation_possible(method),
                    });
                }
                protocol::MSG_AUTH_OK | protocol::MSG_READY => {
                    return Ok(AuthProbeResult {
                        reachable: true,
                        front_door_mode: "direct".to_string(),
                        resolved_host,
                        resolved_port,
                        admitted_methods: Vec::new(),
                        required_method_code: protocol::AUTH_OK,
                        required_method: auth_method_name(protocol::AUTH_OK).to_string(),
                        required_plugin_method_id: auth_plugin_id_for_method(
                            protocol::AUTH_OK,
                            &self.config.auth_method_id,
                        ),
                        required_method_broker_required: false,
                        additional_continuation_possible: false,
                    });
                }
                protocol::MSG_ERROR => return self.raise_protocol_error(&msg.payload),
                _ => continue,
            }
        }
    }

    async fn probe_manager_auth_surface(
        &mut self,
        resolved_host: String,
        resolved_port: u16,
    ) -> Result<AuthProbeResult> {
        let manager_user = if !self.config.manager_username.is_empty() {
            self.config.manager_username.clone()
        } else if !self.config.user.is_empty() {
            self.config.user.clone()
        } else {
            "admin".to_string()
        };

        let hello = {
            let mut out = Vec::with_capacity(4);
            append_u16(&mut out, MCP_PROTOCOL_VERSION);
            append_u16(&mut out, self.config.manager_client_flags);
            out
        };
        self.send_manager_frame(MCP_MSG_HELLO, &hello).await?;
        let (mut msg_type, payload) = self.recv_manager_frame().await?;
        if msg_type != MCP_MSG_STATUS_RESPONSE {
            return Err(Error::with_sqlstate(
                ErrorKind::Connection,
                "expected MCP hello status response",
                Some("08P01".to_string()),
                None,
                None,
            ));
        }

        let mut auth_start = Vec::new();
        append_lpreface(&mut auth_start, &manager_user);
        auth_start.push(MCP_AUTH_METHOD_TOKEN);
        append_u32(&mut auth_start, 0);
        self.send_manager_frame(MCP_MSG_AUTH_START, &auth_start)
            .await?;
        (msg_type, _) = self.recv_manager_frame().await?;
        if msg_type != MCP_MSG_AUTH_CHALLENGE
            && msg_type != MCP_MSG_AUTH_RESPONSE
            && msg_type != MCP_MSG_STATUS_RESPONSE
        {
            return Err(Error::with_sqlstate(
                ErrorKind::Connection,
                "expected MCP auth challenge or auth response",
                Some("08P01".to_string()),
                None,
                None,
            ));
        }

        Ok(AuthProbeResult {
            reachable: true,
            front_door_mode: "manager_proxy".to_string(),
            resolved_host,
            resolved_port,
            admitted_methods: vec![describe_auth_method(
                protocol::AUTH_TOKEN,
                &self.config.auth_method_id,
            )
            .expect("token auth surface")],
            required_method_code: protocol::AUTH_TOKEN,
            required_method: auth_method_name(protocol::AUTH_TOKEN).to_string(),
            required_plugin_method_id: auth_plugin_id_for_method(
                protocol::AUTH_TOKEN,
                &self.config.auth_method_id,
            ),
            required_method_broker_required: false,
            additional_continuation_possible: msg_type == MCP_MSG_AUTH_CHALLENGE,
        })
    }

    async fn handshake(&mut self, params: HashMap<String, String>) -> Result<()> {
        self.authed = false;
        self.parameters.clear();
        let features = self.requested_features();
        let payload = protocol::build_startup_payload(features, &params);
        self.send_message(protocol::MSG_STARTUP, &payload, 0, true)
            .await?;
        let mut scram: Option<ScramExchange> = None;
        let mut active_auth_method = protocol::AUTH_OK;

        loop {
            let msg = self.recv_message().await?;
            match msg.header.msg_type {
                protocol::MSG_NEGOTIATE_VERSION => continue,
                protocol::MSG_AUTH_REQUEST => {
                    let (method, _data) = protocol::parse_auth_request(&msg.payload)?;
                    active_auth_method = method;
                    self.resolved_auth_context.resolved_auth_method =
                        auth_method_name(method).to_string();
                    self.resolved_auth_context.resolved_auth_plugin_id =
                        auth_plugin_id_for_method(method, &self.config.auth_method_id);
                    match method {
                        protocol::AUTH_OK => continue,
                        protocol::AUTH_PASSWORD => {
                            let payload = self.config.password.as_bytes().to_vec();
                            self.send_message(protocol::MSG_AUTH_RESPONSE, &payload, 0, true)
                                .await?;
                        }
                        protocol::AUTH_SCRAM_SHA256 => {
                            if scram.is_none() {
                                scram = Some(ScramExchange::with_algorithm(
                                    &self.config.user,
                                    crate::scram::ScramAlgorithm::Sha256,
                                ));
                            }
                            let exchange = scram.as_mut().unwrap();
                            let payload = exchange.client_first_message().into_bytes();
                            self.send_message(protocol::MSG_AUTH_RESPONSE, &payload, 0, true)
                                .await?;
                        }
                        protocol::AUTH_SCRAM_SHA512 => {
                            if scram.is_none() {
                                scram = Some(ScramExchange::with_algorithm(
                                    &self.config.user,
                                    crate::scram::ScramAlgorithm::Sha512,
                                ));
                            }
                            let exchange = scram.as_mut().unwrap();
                            let payload = exchange.client_first_message().into_bytes();
                            self.send_message(protocol::MSG_AUTH_RESPONSE, &payload, 0, true)
                                .await?;
                        }
                        protocol::AUTH_TOKEN => {
                            let payload = self.resolve_token_auth_payload()?;
                            self.send_message(protocol::MSG_AUTH_RESPONSE, &payload, 0, true)
                                .await?;
                        }
                        protocol::AUTH_MD5 => {
                            return Err(Error::with_sqlstate(
                                ErrorKind::NotSupported,
                                "MD5 authentication is admitted by the server but not executable in the Rust lane",
                                Some("0A000".to_string()),
                                None,
                                None,
                            ));
                        }
                        protocol::AUTH_PEER => {
                            return Err(Error::with_sqlstate(
                                ErrorKind::NotSupported,
                                "PEER authentication requires broker or platform assistance in the Rust lane",
                                Some("0A000".to_string()),
                                None,
                                None,
                            ));
                        }
                        protocol::AUTH_REATTACH => {
                            return Err(Error::with_sqlstate(
                                ErrorKind::NotSupported,
                                "REATTACH authentication negotiation is not executable through the generic Rust auth lane",
                                Some("0A000".to_string()),
                                None,
                                None,
                            ));
                        }
                        _ => {
                            return Err(Error::with_sqlstate(
                                ErrorKind::NotSupported,
                                format!(
                                    "admitted auth method {} requires broker or external ceremony support",
                                    auth_method_name(method)
                                ),
                                Some("0A000".to_string()),
                                None,
                                None,
                            ));
                        }
                    }
                }
                protocol::MSG_AUTH_CONTINUE => {
                    let (method, _stage, data) = protocol::parse_auth_continue(&msg.payload)?;
                    match method {
                        protocol::AUTH_SCRAM_SHA256 | protocol::AUTH_SCRAM_SHA512 => {
                            let exchange = scram.as_mut().ok_or_else(|| {
                                Error::new(ErrorKind::Auth, "SCRAM state missing")
                            })?;
                            let server_first = String::from_utf8_lossy(&data).to_string();
                            let client_final = exchange
                                .handle_server_first(&self.config.password, &server_first)?;
                            self.send_message(
                                protocol::MSG_AUTH_RESPONSE,
                                client_final.as_bytes(),
                                0,
                                true,
                            )
                            .await?;
                        }
                        protocol::AUTH_TOKEN => {
                            let payload = self.resolve_token_auth_payload()?;
                            self.send_message(protocol::MSG_AUTH_RESPONSE, &payload, 0, true)
                                .await?;
                        }
                        _ => {
                            return Err(Error::with_sqlstate(
                                ErrorKind::NotSupported,
                                format!(
                                    "admitted auth continuation {} requires broker or external ceremony support in the Rust lane",
                                    auth_method_name(method)
                                ),
                                Some("0A000".to_string()),
                                None,
                                None,
                            ));
                        }
                    }
                }
                protocol::MSG_AUTH_OK => {
                    let (_session_id, info) = protocol::parse_auth_ok(&msg.payload)?;
                    self.attachment_id
                        .copy_from_slice(&msg.header.attachment_id);
                    self.apply_runtime_txn_id(msg.header.txn_id);
                    self.authed = true;
                    if let Some(ref exchange) = scram {
                        if !info.is_empty() && info.starts_with(b"v=") {
                            let server_final = String::from_utf8_lossy(&info).to_string();
                            exchange.verify_server_final(&server_final)?;
                        }
                    }
                    if self.resolved_auth_context.resolved_auth_method.is_empty()
                        && active_auth_method == protocol::AUTH_OK
                    {
                        self.resolved_auth_context.resolved_auth_method =
                            auth_method_name(protocol::AUTH_OK).to_string();
                        self.resolved_auth_context.resolved_auth_plugin_id =
                            auth_plugin_id_for_method(
                                protocol::AUTH_OK,
                                &self.config.auth_method_id,
                            );
                    }
                }
                protocol::MSG_PARAMETER_STATUS => {
                    for (name, value) in protocol::parse_parameter_statuses(&msg.payload)? {
                        self.handle_parameter_status(name, value);
                    }
                }
                protocol::MSG_READY => {
                    let (status, txn_id, _visibility) = protocol::parse_ready(&msg.payload)?;
                    self.apply_runtime_ready_state(status, txn_id);
                    self.portal_resume_pending = false;
                    self.resolved_auth_context.attached = true;
                    return Ok(());
                }
                protocol::MSG_ERROR => return self.raise_protocol_error(&msg.payload),
                _ => continue,
            }
        }
    }

    fn resolve_token_auth_payload(&self) -> Result<Vec<u8>> {
        if !self.config.auth_token.is_empty() {
            return Ok(self.config.auth_token.as_bytes().to_vec());
        }
        if !self.config.auth_method_payload.is_empty() {
            return Ok(self.config.auth_method_payload.as_bytes().to_vec());
        }
        if !self.config.auth_payload_b64.is_empty() {
            return BASE64_STANDARD
                .decode(self.config.auth_payload_b64.trim())
                .map_err(|_| {
                    Error::with_sqlstate(
                        ErrorKind::Data,
                        "invalid auth_payload_b64 encoding",
                        Some("22023".to_string()),
                        None,
                        None,
                    )
                });
        }
        if !self.config.auth_payload_json.is_empty() {
            return Ok(self.config.auth_payload_json.as_bytes().to_vec());
        }
        if !self.config.workload_identity_token.is_empty() {
            return Ok(self.config.workload_identity_token.as_bytes().to_vec());
        }
        if !self.config.proxy_principal_assertion.is_empty() {
            return Ok(self.config.proxy_principal_assertion.as_bytes().to_vec());
        }
        Err(Error::with_sqlstate(
            ErrorKind::Auth,
            "TOKEN authentication requires auth_token, auth_method_payload, auth_payload_json, auth_payload_b64, workload_identity_token, or proxy_principal_assertion",
            Some("28000".to_string()),
            None,
            None,
        ))
    }

    async fn apply_schema(&mut self) -> Result<()> {
        let schema = self.config.schema.trim();
        if schema.is_empty() || schema.eq_ignore_ascii_case("public") {
            return Ok(());
        }
        let statement = build_schema_statement(schema);
        if statement.is_empty() {
            return Ok(());
        }
        self.send_simple_query(&statement, 0, 0).await?;
        let _ = self.collect_results().await?;
        Ok(())
    }

    async fn collect_results(&mut self) -> Result<QueryResult> {
        let mut columns = Vec::new();
        let mut rows = Vec::new();
        let mut row_count = -1;
        let mut command_tag = String::new();
        let mut ignored_stray_ready = false;

        loop {
            let msg = self.recv_message().await?;
            if self.handle_async_message(&msg)? {
                continue;
            }
            match msg.header.msg_type {
                protocol::MSG_ERROR => return self.raise_protocol_error(&msg.payload),
                protocol::MSG_ROW_DESCRIPTION => {
                    columns = protocol::parse_row_description(&msg.payload)?;
                }
                protocol::MSG_DATA_ROW => {
                    let values = protocol::parse_data_row(&msg.payload, columns.len())?;
                    rows.push(self.decode_row(&columns, &values)?);
                }
                protocol::MSG_COMMAND_COMPLETE => {
                    let (_cmd_type, rows_affected, _last_id, tag) =
                        protocol::parse_command_complete(&msg.payload)?;
                    command_tag = tag;
                    row_count = rows_affected as i64;
                }
                protocol::MSG_PORTAL_SUSPENDED => {
                    self.allow_portal_resume();
                    self.resume_suspended_portal(self.config.fetch_size.max(1))
                        .await?;
                }
                protocol::MSG_READY => {
                    let (status, txn_id, _visibility) = protocol::parse_ready(&msg.payload)?;
                    self.apply_runtime_ready_state(status, txn_id);
                    self.portal_resume_pending = false;
                    if row_count < 0
                        && columns.is_empty()
                        && rows.is_empty()
                        && !ignored_stray_ready
                        && status != 0
                    {
                        ignored_stray_ready = true;
                        continue;
                    }
                    if row_count < 0 {
                        row_count = rows.len() as i64;
                    }
                    let mapped = columns
                        .into_iter()
                        .map(|col| Column {
                            name: col.name,
                            type_oid: col.type_oid,
                            type_modifier: col.type_modifier,
                            format: col.format,
                            nullable: col.nullable,
                        })
                        .collect();
                    return Ok(QueryResult {
                        columns: mapped,
                        rows,
                        row_count,
                        command_tag,
                    });
                }
                _ => continue,
            }
        }
    }

    async fn collect_result_sets(&mut self) -> Result<Vec<ResultSetSummary>> {
        let mut result_sets = Vec::new();
        let mut columns = Vec::new();
        let mut rows = Vec::new();
        let mut saw_result_metadata = false;

        loop {
            let msg = self.recv_message().await?;
            if self.handle_async_message(&msg)? {
                continue;
            }
            match msg.header.msg_type {
                protocol::MSG_ERROR => return self.raise_protocol_error(&msg.payload),
                protocol::MSG_ROW_DESCRIPTION => {
                    columns = protocol::parse_row_description(&msg.payload)?;
                    saw_result_metadata = true;
                }
                protocol::MSG_DATA_ROW => {
                    let values = protocol::parse_data_row(&msg.payload, columns.len())?;
                    rows.push(self.decode_row(&columns, &values)?);
                }
                protocol::MSG_COMMAND_COMPLETE => {
                    let (_cmd_type, rows_affected, last_id, tag) =
                        protocol::parse_command_complete(&msg.payload)?;
                    let row_count = if rows_affected == 0 && !rows.is_empty() {
                        rows.len() as i64
                    } else {
                        saturating_u64_to_i64(rows_affected)
                    };
                    result_sets.push(ResultSetSummary {
                        rows: std::mem::take(&mut rows),
                        row_count,
                        fields: summarize_fields(&columns),
                        command: tag,
                        last_insert_id: saturating_u64_to_i64(last_id),
                    });
                    columns.clear();
                    saw_result_metadata = false;
                }
                protocol::MSG_PORTAL_SUSPENDED => {
                    self.allow_portal_resume();
                    self.resume_suspended_portal(self.config.fetch_size.max(1))
                        .await?;
                }
                protocol::MSG_READY => {
                    let (status, txn_id, _visibility) = protocol::parse_ready(&msg.payload)?;
                    self.apply_runtime_ready_state(status, txn_id);
                    self.portal_resume_pending = false;
                    if saw_result_metadata || !rows.is_empty() {
                        let pending_row_count = rows.len() as i64;
                        result_sets.push(ResultSetSummary {
                            rows: std::mem::take(&mut rows),
                            row_count: pending_row_count,
                            fields: summarize_fields(&columns),
                            command: String::new(),
                            last_insert_id: 0,
                        });
                    }
                    return Ok(result_sets);
                }
                _ => continue,
            }
        }
    }

    async fn drain_until_ready(&mut self) -> Result<()> {
        loop {
            let msg = self.recv_message().await?;
            if self.handle_async_message(&msg)? {
                continue;
            }
            match msg.header.msg_type {
                protocol::MSG_READY => {
                    let (status, txn_id, _visibility) = protocol::parse_ready(&msg.payload)?;
                    self.apply_runtime_ready_state(status, txn_id);
                    return Ok(());
                }
                protocol::MSG_ERROR => return self.raise_protocol_error(&msg.payload),
                _ => continue,
            }
        }
    }

    fn handle_parameter_status(&mut self, name: String, value: String) {
        if name == "attachment_id" {
            if let Some(parsed) = parse_uuid_bytes(&value) {
                self.attachment_id = parsed;
            }
        }
        if name == "current_txn_id" {
            if let Ok(parsed) = value.trim().parse::<u64>() {
                self.apply_runtime_txn_id(parsed);
            }
        }
        self.parameters.insert(name, value);
    }

    fn handle_async_message(&mut self, msg: &protocol::Message) -> Result<bool> {
        match msg.header.msg_type {
            protocol::MSG_PARAMETER_STATUS => {
                for (name, value) in protocol::parse_parameter_statuses(&msg.payload)? {
                    self.handle_parameter_status(name, value);
                }
                Ok(true)
            }
            protocol::MSG_NOTIFICATION => {
                let notice = protocol::parse_notification(&msg.payload)?;
                for handler in &self.notification_handlers {
                    handler(&notice);
                }
                Ok(true)
            }
            protocol::MSG_QUERY_PLAN => {
                self.last_plan = Some(protocol::parse_query_plan(&msg.payload)?);
                Ok(true)
            }
            protocol::MSG_SBLR_COMPILED => {
                self.last_sblr = Some(protocol::parse_sblr_compiled(&msg.payload)?);
                Ok(true)
            }
            protocol::MSG_TXN_STATUS => {
                let (status, txn_id) = protocol::parse_txn_status(&msg.payload)?;
                if status == b'T' {
                    self.txn_id = txn_id;
                    self.runtime_txn_active = true;
                } else {
                    self.clear_transaction_state();
                }
                Ok(true)
            }
            _ => Ok(false),
        }
    }

    async fn send_simple_query(&mut self, sql: &str, max_rows: u32, timeout_ms: u32) -> Result<()> {
        self.send_simple_query_with_flags(sql, max_rows, timeout_ms, 0)
            .await
    }

    async fn send_simple_query_with_flags(
        &mut self,
        sql: &str,
        max_rows: u32,
        timeout_ms: u32,
        extra_flags: u32,
    ) -> Result<()> {
        let flags = if self.config.binary_transfer {
            QUERY_FLAG_BINARY_RESULT
        } else {
            0
        } | extra_flags;
        let payload = protocol::build_query_payload(sql, flags, max_rows, timeout_ms);
        self.last_plan = None;
        self.last_sblr = None;
        self.portal_resume_pending = false;
        let sequence = self
            .send_message(protocol::MSG_QUERY, &payload, 0, false)
            .await?;
        self.last_query_sequence = sequence;
        Ok(())
    }

    async fn send_extended_query(
        &mut self,
        sql: &str,
        params: &[Param],
        max_rows: u32,
    ) -> Result<()> {
        let mut param_values = Vec::with_capacity(params.len());
        let mut param_types = Vec::with_capacity(params.len());
        for param in params {
            let (value, oid) = encode_param(param)?;
            param_values.push(value);
            param_types.push(oid);
        }
        let parse_payload = protocol::build_parse_payload("", sql, &param_types);
        self.send_message(protocol::MSG_PARSE, &parse_payload, 0, false)
            .await?;
        let described = self.describe_statement("").await?;
        if described > 0 && described != params.len() {
            return Err(Error::with_sqlstate(
                ErrorKind::Syntax,
                "parameter count mismatch",
                Some("07001".to_string()),
                None,
                None,
            ));
        }

        let result_formats = if self.config.binary_transfer {
            vec![FORMAT_BINARY]
        } else {
            Vec::new()
        };
        let bind_payload = protocol::build_bind_payload("", "", &param_values, &result_formats);
        self.send_message(protocol::MSG_BIND, &bind_payload, 0, false)
            .await?;

        let exec_payload = protocol::build_execute_payload("", max_rows);
        self.last_plan = None;
        self.last_sblr = None;
        self.portal_resume_pending = false;
        let sequence = self
            .send_message(protocol::MSG_EXECUTE, &exec_payload, 0, false)
            .await?;
        self.last_query_sequence = sequence;
        if max_rows == 0 {
            self.send_message(protocol::MSG_SYNC, &[], 0, false).await?;
        }
        Ok(())
    }

    async fn describe_statement(&mut self, statement_name: &str) -> Result<usize> {
        let payload = protocol::build_describe_payload(b'S', statement_name);
        self.send_message(protocol::MSG_DESCRIBE, &payload, 0, false)
            .await?;
        self.send_message(protocol::MSG_SYNC, &[], 0, false).await?;
        let mut param_count = 0usize;
        loop {
            let msg = self.recv_message().await?;
            if self.handle_async_message(&msg)? {
                continue;
            }
            match msg.header.msg_type {
                protocol::MSG_PARAMETER_DESCRIPTION => {
                    let types = protocol::parse_parameter_description(&msg.payload)?;
                    param_count = types.len();
                }
                protocol::MSG_ERROR => return self.raise_protocol_error(&msg.payload),
                protocol::MSG_READY => {
                    let (status, txn_id, _visibility) = protocol::parse_ready(&msg.payload)?;
                    self.apply_runtime_ready_state(status, txn_id);
                    self.portal_resume_pending = false;
                    return Ok(param_count);
                }
                _ => continue,
            }
        }
    }

    async fn connect_transport(&self) -> Result<Box<dyn AsyncReadWrite>> {
        let transport_mode = self
            .config
            .transport_mode
            .trim()
            .to_ascii_lowercase()
            .replace('-', "_");
        if matches!(transport_mode.as_str(), "local" | "ipc" | "local_ipc") {
            #[cfg(unix)]
            {
                if self.config.ipc_path.trim().is_empty() {
                    return Err(Error::new(
                        ErrorKind::Connection,
                        "ipc_path is required for local_ipc",
                    ));
                }
                let stream = timeout(
                    Duration::from_millis(self.config.connect_timeout_ms),
                    UnixStream::connect(&self.config.ipc_path),
                )
                .await
                .map_err(|_| Error::new(ErrorKind::Connection, "connect timeout"))?
                .map_err(|e| Error::new(ErrorKind::Connection, e.to_string()))?;
                return Ok(Box::new(stream));
            }
            #[cfg(not(unix))]
            {
                return Err(Error::new(
                    ErrorKind::NotSupported,
                    "local_ipc requires native IPC support on this platform",
                ));
            }
        }
        let addr = format!("{}:{}", self.config.host, self.config.port);
        let timeout_ms = self.config.connect_timeout_ms;
        let stream = timeout(Duration::from_millis(timeout_ms), TcpStream::connect(&addr))
            .await
            .map_err(|_| Error::new(ErrorKind::Connection, "connect timeout"))?
            .map_err(|e| Error::new(ErrorKind::Connection, e.to_string()))?;
        stream.set_nodelay(true).ok();

        let sslmode = self.config.sslmode.to_ascii_lowercase();
        if sslmode == "disable" {
            return Ok(Box::new(stream));
        }
        let tls = self.connect_tls(stream).await?;
        Ok(Box::new(tls))
    }

    async fn connect_tls(&self, stream: TcpStream) -> Result<TlsStream<TcpStream>> {
        use rustls::{ClientConfig, RootCertStore};

        let mut root_store = RootCertStore::empty();
        if let Ok(store) = rustls_native_certs::load_native_certs() {
            for cert in store {
                root_store.add(&rustls::Certificate(cert.0)).ok();
            }
        }
        if let Some(ref path) = self.config.sslrootcert {
            let data = std::fs::read(path)
                .map_err(|e| Error::new(ErrorKind::Connection, e.to_string()))?;
            let mut cursor = std::io::Cursor::new(data);
            let certs = rustls_pemfile::certs(&mut cursor).unwrap_or_default();
            for cert in certs {
                root_store.add(&rustls::Certificate(cert)).ok();
            }
        }

        let builder = ClientConfig::builder()
            .with_safe_defaults()
            .with_root_certificates(root_store);

        let mut client_config = if let (Some(cert_path), Some(key_path)) =
            (&self.config.sslcert, &self.config.sslkey)
        {
            let cert_bytes = std::fs::read(cert_path)
                .map_err(|e| Error::new(ErrorKind::Connection, e.to_string()))?;
            let key_bytes = std::fs::read(key_path)
                .map_err(|e| Error::new(ErrorKind::Connection, e.to_string()))?;
            let mut cert_cursor = std::io::Cursor::new(cert_bytes);
            let mut key_cursor = std::io::Cursor::new(key_bytes);
            let certs = rustls_pemfile::certs(&mut cert_cursor).unwrap_or_default();
            let mut keys = rustls_pemfile::pkcs8_private_keys(&mut key_cursor).unwrap_or_default();
            if keys.is_empty() {
                key_cursor.set_position(0);
                keys = rustls_pemfile::rsa_private_keys(&mut key_cursor).unwrap_or_default();
            }
            if !certs.is_empty() && !keys.is_empty() {
                builder
                    .with_single_cert(
                        certs.into_iter().map(rustls::Certificate).collect(),
                        rustls::PrivateKey(keys.remove(0)),
                    )
                    .map_err(|e| Error::new(ErrorKind::Connection, e.to_string()))?
            } else {
                builder.with_no_client_auth()
            }
        } else {
            builder.with_no_client_auth()
        };

        let sslmode = self.config.sslmode.to_ascii_lowercase();
        if matches!(sslmode.as_str(), "allow" | "prefer" | "require") {
            client_config
                .dangerous()
                .set_certificate_verifier(Arc::new(NoVerifier));
        }

        let connector = TlsConnector::from(Arc::new(client_config));
        let server_name = rustls::ServerName::try_from(self.config.host.as_str())
            .map_err(|_| Error::new(ErrorKind::Connection, "invalid tls server name"))?;
        let tls = connector
            .connect(server_name, stream)
            .await
            .map_err(|e| Error::new(ErrorKind::Connection, e.to_string()))?;
        Ok(tls)
    }

    async fn send_message(
        &mut self,
        msg_type: u8,
        payload: &[u8],
        flags: u8,
        force_zero: bool,
    ) -> Result<u32> {
        let stream = self
            .stream
            .as_mut()
            .ok_or_else(|| Error::new(ErrorKind::Connection, "no active socket"))?;
        let sequence = self.sequence;
        self.sequence = self.sequence.wrapping_add(1);
        let header = MessageHeader {
            msg_type,
            flags,
            length: payload.len() as u32,
            sequence,
            attachment_id: if self.authed && !force_zero {
                self.attachment_id
            } else {
                [0u8; 16]
            },
            txn_id: if self.authed && !force_zero {
                self.txn_id
            } else {
                0
            },
        };
        let data = protocol::encode_message(&header, payload);
        if self.config.socket_timeout_ms > 0 {
            timeout(
                Duration::from_millis(self.config.socket_timeout_ms),
                stream.write_all(&data),
            )
            .await
            .map_err(|_| Error::new(ErrorKind::Connection, "socket write timeout"))??;
        } else {
            stream.write_all(&data).await?;
        }
        Ok(sequence)
    }

    async fn recv_message(&mut self) -> Result<protocol::Message> {
        let mut header_bytes = [0u8; protocol::HEADER_SIZE];
        self.read_exact(&mut header_bytes).await?;
        let header = protocol::decode_header(&header_bytes)?;
        let mut payload = vec![0u8; header.length as usize];
        if header.length > 0 {
            self.read_exact(&mut payload).await?;
        }
        Ok(protocol::Message { header, payload })
    }

    async fn read_exact(&mut self, buf: &mut [u8]) -> Result<()> {
        let stream = self
            .stream
            .as_mut()
            .ok_or_else(|| Error::new(ErrorKind::Connection, "no active socket"))?;
        if self.config.socket_timeout_ms > 0 {
            timeout(
                Duration::from_millis(self.config.socket_timeout_ms),
                stream.read_exact(buf),
            )
            .await
            .map_err(|_| Error::new(ErrorKind::Connection, "socket read timeout"))??;
        } else {
            stream.read_exact(buf).await?;
        }
        Ok(())
    }

    fn decode_row(
        &self,
        columns: &[protocol::ColumnInfo],
        values: &[protocol::ColumnValue],
    ) -> Result<Vec<Value>> {
        let mut row = Vec::with_capacity(values.len());
        for (idx, value) in values.iter().enumerate() {
            let col = columns.get(idx);
            let type_oid = col.map(|c| c.type_oid).unwrap_or(0);
            let format = col.map(|c| c.format as u16).unwrap_or(FORMAT_BINARY);
            row.push(decode_value(type_oid, value.data.clone(), format)?);
        }
        Ok(row)
    }

    fn raise_protocol_error<T>(&self, payload: &[u8]) -> Result<T> {
        let (_severity, sqlstate, message, detail, hint) = protocol::parse_error_message(payload)?;
        let mut parts = Vec::new();
        if !message.is_empty() {
            parts.push(message.clone());
        }
        if !detail.is_empty() {
            parts.push(format!("DETAIL: {}", detail));
        }
        if !hint.is_empty() {
            parts.push(format!("HINT: {}", hint));
        }
        let combined = if parts.is_empty() {
            "query failed".to_string()
        } else {
            parts.join("\n")
        };
        Err(error_from_sqlstate(
            &sqlstate,
            combined,
            Some(detail),
            Some(hint),
        ))
    }

    fn ensure_connected(&self) -> Result<()> {
        if !self.connected {
            return Err(Error::new(ErrorKind::Connection, "client is not connected"));
        }
        Ok(())
    }

    fn has_active_transaction(&self) -> bool {
        self.runtime_txn_active || self.txn_id != 0
    }

    fn ensure_no_active_transaction(&self) -> Result<()> {
        if self.has_active_transaction() {
            return Err(Self::invalid_txn_state("transaction already active"));
        }
        Ok(())
    }

    fn ensure_transaction_active(&self, operation: &str) -> Result<()> {
        if !self.has_active_transaction() {
            return Err(Self::invalid_txn_state(format!(
                "cannot {} without an active transaction",
                operation
            )));
        }
        Ok(())
    }

    fn invalid_txn_state(message: impl Into<String>) -> Error {
        Error::with_sqlstate(
            ErrorKind::Transaction,
            message,
            Some("25000".to_string()),
            None,
            None,
        )
    }

    fn validate_savepoint_name(name: &str) -> Result<&str> {
        let trimmed = name.trim();
        if trimmed.is_empty() {
            return Err(Error::with_sqlstate(
                ErrorKind::Syntax,
                "savepoint name is required",
                Some("42601".to_string()),
                None,
                None,
            ));
        }
        Ok(trimmed)
    }

    fn quote_string_literal(value: &str) -> String {
        format!("'{}'", value.replace('\'', "''"))
    }

    fn build_prepared_transaction_sql(verb: &str, global_transaction_id: &str) -> Result<String> {
        let normalized = global_transaction_id.trim();
        if normalized.is_empty() {
            return Err(Error::with_sqlstate(
                ErrorKind::Syntax,
                "global transaction id is required",
                Some("42601".to_string()),
                None,
                None,
            ));
        }
        Ok(format!(
            "{} {}",
            verb,
            Self::quote_string_literal(normalized)
        ))
    }

    fn allow_portal_resume(&mut self) {
        self.portal_resume_pending = true;
    }

    async fn resume_suspended_portal(&mut self, page_size: u32) -> Result<()> {
        if !self.portal_resume_pending {
            return Err(Error::with_sqlstate(
                ErrorKind::Transaction,
                "portal resume requires explicit suspended state",
                Some("55000".to_string()),
                None,
                None,
            ));
        }
        self.portal_resume_pending = false;
        let payload = protocol::build_execute_payload("", page_size.max(1));
        let sequence = self
            .send_message(protocol::MSG_EXECUTE, &payload, 0, false)
            .await?;
        self.last_query_sequence = sequence;
        Ok(())
    }

    fn can_adopt_fresh_native_boundary(opts: &TxnBeginOptions) -> bool {
        matches!(
            opts.isolation_level,
            None | Some(protocol::ISOLATION_READ_COMMITTED)
        ) && matches!(
            opts.read_committed_mode,
            None | Some(protocol::READ_COMMITTED_MODE_DEFAULT)
        ) && opts.access_mode.is_none()
            && opts.deferrable.is_none()
            && opts.wait.is_none()
            && opts.timeout_ms.is_none()
            && opts.autocommit_mode.is_none()
            && opts.conflict_action == 0
    }

    fn apply_runtime_txn_id(&mut self, txn_id: u64) {
        self.txn_id = txn_id;
        if txn_id != 0 {
            self.runtime_txn_active = true;
        }
    }

    fn apply_runtime_ready_state(&mut self, status: u8, txn_id: u64) {
        self.txn_id = txn_id;
        if status != 0 {
            // READY is authoritative for native engine session activity. A
            // fresh MGA boundary may be active while the wire header still
            // reports txn_id == 0.
            self.runtime_txn_active = true;
            return;
        }
        self.clear_transaction_state();
    }

    fn clear_transaction_state(&mut self) {
        self.txn_id = 0;
        self.runtime_txn_active = false;
        self.explicit_transaction = false;
    }

    async fn drain_immediate_reopen_boundary(&mut self) -> Result<()> {
        tokio::task::yield_now().await;
        loop {
            match timeout(Duration::from_millis(5), self.recv_message()).await {
                Ok(Ok(msg)) => {
                    if self.handle_async_message(&msg)? {
                        continue;
                    }
                    match msg.header.msg_type {
                        protocol::MSG_READY => {
                            let (status, txn_id, _visibility) =
                                protocol::parse_ready(&msg.payload)?;
                            self.apply_runtime_ready_state(status, txn_id);
                            self.portal_resume_pending = false;
                            continue;
                        }
                        protocol::MSG_ERROR => return self.raise_protocol_error(&msg.payload),
                        _ => return Ok(()),
                    }
                }
                Ok(Err(err)) => return Err(err),
                Err(_) => return Ok(()),
            }
        }
    }

    fn requested_features(&self) -> u64 {
        let mut features = 0u64;
        if self.config.compression.eq_ignore_ascii_case("zstd") {
            features |= protocol::FEATURE_COMPRESSION;
        }
        if self.config.binary_transfer {
            features |= protocol::FEATURE_STREAMING;
            features |= protocol::FEATURE_BINARY_COPY;
        }
        features |= protocol::FEATURE_SAVEPOINTS;
        features |= protocol::FEATURE_BATCH;
        features |= protocol::FEATURE_PIPELINE;
        features
    }

    async fn begin_operation(&mut self, name: &str) -> Result<Option<SpanContext>> {
        if !self.circuit_breaker.allow_request().await {
            return Err(Error::new(ErrorKind::Connection, "circuit breaker is OPEN"));
        }
        if self.keepalive_tracker.needs_validation().await {
            self.ping().await?;
        }
        self.keepalive_tracker.mark_active().await;
        Ok(self.telemetry.start_span(name).await)
    }

    async fn end_operation(&self, span: Option<SpanContext>, success: bool) {
        if success {
            self.circuit_breaker.record_success().await;
        } else {
            self.circuit_breaker.record_failure().await;
        }
        if let Some(span) = span {
            self.telemetry.end_span(span, success).await;
        }
    }
}

pub async fn probe_auth_surface(dsn: &str) -> Result<AuthProbeResult> {
    let config = Config::from_dsn(dsn)?;
    let mut client = Client::new(config);
    client.probe_auth_surface().await
}

impl<'a> QueryStream<'a> {
    pub async fn next_row(&mut self) -> Result<Option<Vec<Value>>> {
        if self.done {
            return Ok(None);
        }
        loop {
            let msg = self.client.recv_message().await?;
            if self.client.handle_async_message(&msg)? {
                continue;
            }
            match msg.header.msg_type {
                protocol::MSG_ERROR => {
                    let err = self.client.raise_protocol_error(&msg.payload);
                    self.finalize(false).await;
                    return err;
                }
                protocol::MSG_ROW_DESCRIPTION => {
                    self.columns = protocol::parse_row_description(&msg.payload)?;
                }
                protocol::MSG_DATA_ROW => {
                    let values = protocol::parse_data_row(&msg.payload, self.columns.len())?;
                    let row = self.client.decode_row(&self.columns, &values)?;
                    return Ok(Some(row));
                }
                protocol::MSG_COMMAND_COMPLETE => {
                    let (_cmd_type, rows_affected, _last_id, tag) =
                        protocol::parse_command_complete(&msg.payload)?;
                    self.command_tag = tag;
                    self.row_count = rows_affected as i64;
                }
                protocol::MSG_PORTAL_SUSPENDED => {
                    self.client.allow_portal_resume();
                    self.client.resume_suspended_portal(self.page_size).await?;
                }
                protocol::MSG_READY => {
                    let (status, txn_id, _visibility) = protocol::parse_ready(&msg.payload)?;
                    self.client.apply_runtime_ready_state(status, txn_id);
                    self.client.portal_resume_pending = false;
                    self.done = true;
                    self.finalize(true).await;
                    return Ok(None);
                }
                _ => {}
            }
        }
    }

    async fn finalize(&mut self, success: bool) {
        if self.finalized {
            return;
        }
        self.finalized = true;
        if success {
            self.circuit_breaker.record_success().await;
        } else {
            self.circuit_breaker.record_failure().await;
        }
        if let Some(span) = self.span.take() {
            self.telemetry.end_span(span, success).await;
        }
    }

    pub fn columns(&self) -> &[protocol::ColumnInfo] {
        &self.columns
    }

    pub fn row_count(&self) -> i64 {
        self.row_count
    }

    pub fn command_tag(&self) -> &str {
        &self.command_tag
    }
}

struct NoVerifier;

impl rustls::client::ServerCertVerifier for NoVerifier {
    fn verify_server_cert(
        &self,
        _end_entity: &rustls::Certificate,
        _intermediates: &[rustls::Certificate],
        _server_name: &rustls::ServerName,
        _scts: &mut dyn Iterator<Item = &[u8]>,
        _ocsp_response: &[u8],
        _now: std::time::SystemTime,
    ) -> std::result::Result<rustls::client::ServerCertVerified, rustls::Error> {
        Ok(rustls::client::ServerCertVerified::assertion())
    }
}

fn build_schema_statement(schema: &str) -> String {
    let trimmed = schema.trim();
    if trimmed.is_empty() {
        return String::new();
    }
    if trimmed.contains(',') {
        let parts: Vec<String> = trimmed
            .split(',')
            .map(|part| part.trim())
            .filter(|part| !part.is_empty())
            .map(quote_identifier)
            .collect();
        if parts.is_empty() {
            return String::new();
        }
        return format!("SET SEARCH_PATH TO {}", parts.join(", "));
    }
    format!("SET SCHEMA {}", quote_identifier(trimmed))
}

fn quote_identifier(name: &str) -> String {
    format!("\"{}\"", name.replace('"', "\"\""))
}

fn summarize_fields(columns: &[protocol::ColumnInfo]) -> Vec<FieldSummary> {
    columns
        .iter()
        .map(|col| FieldSummary {
            name: col.name.clone(),
            type_oid: col.type_oid,
            format: col.format as u16,
            nullable: col.nullable,
        })
        .collect()
}

#[derive(Debug, Clone)]
struct MetadataRestrictionBinding {
    indexes: Vec<usize>,
    expected: String,
    expect_null: bool,
}

fn apply_metadata_restrictions(
    mut result: QueryResult,
    restrictions: &HashMap<String, String>,
    collection: Option<&str>,
) -> QueryResult {
    if restrictions.is_empty() {
        return result;
    }

    let mut allowed_aliases = HashSet::new();
    if let Some(collection_name) = collection {
        if let Some(resolved_collection) = normalize_metadata_collection_name(collection_name) {
            for key in metadata_collection_restriction_keys(resolved_collection) {
                for alias in metadata_restriction_key_aliases(key) {
                    let normalized_alias = normalize_metadata_identifier(&alias);
                    if !normalized_alias.is_empty() {
                        allowed_aliases.insert(normalized_alias);
                    }
                }
            }
        }
    }

    let mut column_index: HashMap<String, Vec<usize>> = HashMap::new();
    for (idx, column) in result.columns.iter().enumerate() {
        column_index
            .entry(normalize_metadata_identifier(&column.name))
            .or_default()
            .push(idx);
    }

    let mut bindings = Vec::new();
    for (key, value) in restrictions {
        let key = key.trim();
        let value = value.trim();
        if key.is_empty() || value.is_empty() {
            continue;
        }

        let normalized_key = normalize_metadata_identifier(key);
        if normalized_key.is_empty() {
            continue;
        }

        let mut aliases = HashSet::new();
        aliases.insert(normalized_key.clone());
        for alias in metadata_restriction_key_aliases(key) {
            let normalized_alias = normalize_metadata_identifier(&alias);
            if normalized_alias.is_empty() {
                continue;
            }
            if !allowed_aliases.is_empty()
                && !allowed_aliases.contains(&normalized_alias)
                && normalized_alias != normalized_key
            {
                continue;
            }
            aliases.insert(normalized_alias);
        }
        if aliases.is_empty() {
            continue;
        }

        let mut index_set = HashSet::new();
        for alias in aliases {
            if let Some(indexes) = column_index.get(&alias) {
                for index in indexes {
                    index_set.insert(*index);
                }
            }
        }
        if index_set.is_empty() {
            continue;
        }
        let indexes = index_set.into_iter().collect::<Vec<_>>();

        let normalized = normalize_metadata_match_text(value);
        bindings.push(MetadataRestrictionBinding {
            indexes,
            expected: normalized.clone(),
            expect_null: normalized == "null",
        });
    }

    if bindings.is_empty() {
        return result;
    }

    result.rows.retain(|row| {
        bindings.iter().all(|binding| {
            let mut matched = false;
            for index in &binding.indexes {
                let Some(value) = row.get(*index) else {
                    continue;
                };

                if binding.expect_null {
                    if matches!(value, Value::Null) {
                        matched = true;
                        break;
                    }
                    continue;
                }

                if metadata_value_text(value)
                    .map(|text| normalize_metadata_match_text(&text) == binding.expected)
                    .unwrap_or(false)
                {
                    matched = true;
                    break;
                }
            }
            matched
        })
    });
    result.row_count = saturating_u64_to_i64(result.rows.len() as u64);
    result
}

fn metadata_collection_restriction_keys(collection: &str) -> &'static [&'static str] {
    match collection {
        "catalogs" => &["catalog"],
        "schemas" => &["catalog", "schema"],
        "tables" => &["catalog", "schema", "table", "type"],
        "columns" => &["catalog", "schema", "table", "column", "type"],
        "indexes" => &["catalog", "schema", "table", "index"],
        "index_columns" => &["catalog", "schema", "table", "index", "column"],
        "constraints" => &["catalog", "schema", "table", "constraint"],
        "primary_keys" => &["catalog", "schema", "table", "constraint"],
        "foreign_keys" => &["catalog", "schema", "table", "constraint"],
        "table_privileges" => &["catalog", "schema", "table"],
        "column_privileges" => &["catalog", "schema", "table", "column"],
        "procedures" => &["catalog", "schema", "procedure"],
        "functions" => &["catalog", "schema", "function"],
        "routines" => &["catalog", "schema", "routine"],
        "type_info" => &["type"],
        _ => &[],
    }
}

fn metadata_restriction_key_aliases(key: &str) -> Vec<String> {
    match normalize_metadata_identifier(key).as_str() {
        "catalog" | "catalogs" | "tablecat" | "tablecatalog" | "catalogname" => vec![
            "catalog_name".to_string(),
            "table_cat".to_string(),
            "table_catalog".to_string(),
            "TABLE_CAT".to_string(),
            "TABLE_CATALOG".to_string(),
        ],
        "schema" | "schemas" | "tableschem" | "tableschema" | "schemaname" => vec![
            "schema_name".to_string(),
            "table_schem".to_string(),
            "table_schema".to_string(),
            "TABLE_SCHEM".to_string(),
            "TABLE_SCHEMA".to_string(),
        ],
        "table" | "tables" | "tablename" => {
            vec!["table_name".to_string(), "TABLE_NAME".to_string()]
        }
        "column" | "columns" | "columnname" => {
            vec!["column_name".to_string(), "COLUMN_NAME".to_string()]
        }
        "index" | "indexes" | "indexname" => {
            vec!["index_name".to_string(), "INDEX_NAME".to_string()]
        }
        "constraint" | "constraints" | "constraintname" => {
            vec!["constraint_name".to_string(), "CONSTRAINT_NAME".to_string()]
        }
        "procedure" | "procedures" | "procedurename" => vec![
            "procedure_name".to_string(),
            "routine_name".to_string(),
            "PROCEDURE_NAME".to_string(),
            "ROUTINE_NAME".to_string(),
        ],
        "function" | "functions" | "functionname" => vec![
            "function_name".to_string(),
            "routine_name".to_string(),
            "FUNCTION_NAME".to_string(),
            "ROUTINE_NAME".to_string(),
        ],
        "routine" | "routines" | "routinename" => vec![
            "routine_name".to_string(),
            "procedure_name".to_string(),
            "function_name".to_string(),
            "ROUTINE_NAME".to_string(),
            "PROCEDURE_NAME".to_string(),
            "FUNCTION_NAME".to_string(),
        ],
        "type" | "types" | "typename" | "typedataname" => vec![
            "data_type_name".to_string(),
            "type_name".to_string(),
            "DATA_TYPE_NAME".to_string(),
            "TYPE_NAME".to_string(),
        ],
        _ => Vec::new(),
    }
}

fn normalize_metadata_identifier(value: &str) -> String {
    let mut out = String::with_capacity(value.len());
    for ch in value.chars() {
        if ch.is_ascii_alphanumeric() {
            out.push(ch.to_ascii_lowercase());
        }
    }
    out
}

fn normalize_metadata_match_text(value: &str) -> String {
    value.trim().to_ascii_lowercase()
}

fn metadata_value_text(value: &Value) -> Option<String> {
    match value {
        Value::Null => None,
        Value::Bool(v) => Some(v.to_string()),
        Value::Int16(v) => Some(v.to_string()),
        Value::Int32(v) => Some(v.to_string()),
        Value::Int64(v) => Some(v.to_string()),
        Value::Float32(v) => Some(v.to_string()),
        Value::Float64(v) => Some(v.to_string()),
        Value::Decimal(v) => Some(v.to_string()),
        Value::String(v) => Some(v.clone()),
        Value::Bytes(v) => Some(String::from_utf8_lossy(v).to_string()),
        Value::Date(v) => Some(v.to_string()),
        Value::Time(v) => Some(v.to_string()),
        Value::Timestamp(v) => Some(v.to_rfc3339()),
        Value::Interval(v) => Some(format!("{}:{}:{}", v.months, v.days, v.micros)),
        Value::Uuid(v) => Some(v.clone()),
        Value::Json(v) => Some(v.to_string()),
        Value::Jsonb(v) => Some(String::from_utf8_lossy(&v.raw).to_string()),
        Value::Vector(v) => Some(
            v.iter()
                .map(|value| value.to_string())
                .collect::<Vec<_>>()
                .join(","),
        ),
        Value::Array(_) | Value::Range(_) | Value::Geometry(_) | Value::Composite(_) => None,
    }
}

fn saturating_u64_to_i64(value: u64) -> i64 {
    i64::try_from(value).unwrap_or(i64::MAX)
}

fn parse_uuid_bytes(value: &str) -> Option<[u8; 16]> {
    let hex: String = value.chars().filter(|c| *c != '-').collect();
    if hex.len() != 32 || !hex.chars().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    let mut bytes = [0u8; 16];
    for i in 0..16 {
        let start = i * 2;
        let part = &hex[start..start + 2];
        if let Ok(byte) = u8::from_str_radix(part, 16) {
            bytes[i] = byte;
        } else {
            return None;
        }
    }
    Some(bytes)
}

fn query_result_to_metadata_rows(result: &QueryResult) -> Vec<MetadataRow> {
    let mut rows = Vec::with_capacity(result.rows.len());
    for row in &result.rows {
        let mut metadata_row = MetadataRow::with_capacity(result.columns.len());
        for (index, column) in result.columns.iter().enumerate() {
            let value = row.get(index).cloned().unwrap_or(Value::Null);
            metadata_row.insert(column.name.clone(), value_to_json(&value));
        }
        rows.push(metadata_row);
    }
    rows
}

fn filter_schema_metadata_rows_by_pattern(
    rows: Vec<MetadataRow>,
    schema_pattern: Option<&str>,
) -> Vec<MetadataRow> {
    let Some(pattern) = schema_pattern
        .map(str::trim)
        .filter(|value| !value.is_empty())
    else {
        return rows;
    };

    rows.into_iter()
        .filter(|row| {
            metadata_row_schema_name(row)
                .map(|schema| like_match(schema, pattern))
                .unwrap_or(false)
        })
        .collect()
}

fn metadata_row_schema_name(row: &MetadataRow) -> Option<&str> {
    const CANDIDATES: [&str; 6] = [
        "schema_name",
        "TABLE_SCHEM",
        "table_schem",
        "table_schema",
        "TABLE_SCHEMA",
        "schema",
    ];
    for candidate in CANDIDATES {
        if let Some(value) = row.get(candidate).or_else(|| {
            row.iter()
                .find_map(|(key, value)| key.eq_ignore_ascii_case(candidate).then_some(value))
        }) {
            if let Some(text) = value.as_str() {
                return Some(text);
            }
        }
    }
    None
}

fn like_match(value: &str, pattern: &str) -> bool {
    let value = value.to_ascii_lowercase();
    let pattern = pattern.to_ascii_lowercase();
    let value_chars: Vec<char> = value.chars().collect();
    let pattern_chars: Vec<char> = pattern.chars().collect();
    like_match_chars(&value_chars, &pattern_chars, 0, 0)
}

fn like_match_chars(value: &[char], pattern: &[char], vi: usize, pi: usize) -> bool {
    if pi >= pattern.len() {
        return vi >= value.len();
    }

    match pattern[pi] {
        '\\' => {
            if pi + 1 >= pattern.len() {
                return vi >= value.len();
            }
            if vi >= value.len() || value[vi] != pattern[pi + 1] {
                return false;
            }
            like_match_chars(value, pattern, vi + 1, pi + 2)
        }
        '%' => {
            let mut idx = vi;
            while idx <= value.len() {
                if like_match_chars(value, pattern, idx, pi + 1) {
                    return true;
                }
                idx += 1;
            }
            false
        }
        '_' => {
            if vi >= value.len() {
                false
            } else {
                like_match_chars(value, pattern, vi + 1, pi + 1)
            }
        }
        literal => {
            if vi >= value.len() || value[vi] != literal {
                false
            } else {
                like_match_chars(value, pattern, vi + 1, pi + 1)
            }
        }
    }
}

fn schema_tree_nodes_to_payload(nodes: &[MetadataSchemaTreeNode]) -> Vec<JsonValue> {
    nodes
        .iter()
        .map(|node| {
            json!({
                "name": node.name,
                "path": node.path,
                "terminal": node.terminal,
                "children": schema_tree_nodes_to_payload(&node.children),
            })
        })
        .collect()
}

fn value_to_json(value: &Value) -> JsonValue {
    match value {
        Value::Null => JsonValue::Null,
        Value::Bool(v) => JsonValue::from(*v),
        Value::Int16(v) => JsonValue::from(*v),
        Value::Int32(v) => JsonValue::from(*v),
        Value::Int64(v) => JsonValue::from(*v),
        Value::Float32(v) => JsonValue::from(*v),
        Value::Float64(v) => JsonValue::from(*v),
        Value::Decimal(v) => JsonValue::String(v.to_string()),
        Value::String(v) => JsonValue::String(v.clone()),
        Value::Bytes(v) => JsonValue::String(String::from_utf8_lossy(v).to_string()),
        Value::Date(v) => JsonValue::String(v.to_string()),
        Value::Time(v) => JsonValue::String(v.to_string()),
        Value::Timestamp(v) => JsonValue::String(v.to_rfc3339()),
        Value::Interval(v) => JsonValue::String(format!("{}:{}:{}", v.months, v.days, v.micros)),
        Value::Uuid(v) => JsonValue::String(v.clone()),
        Value::Json(v) => v.clone(),
        Value::Jsonb(v) => serde_json::from_slice(&v.raw)
            .unwrap_or_else(|_| JsonValue::String(String::from_utf8_lossy(&v.raw).to_string())),
        Value::Vector(v) => JsonValue::Array(v.iter().map(|item| JsonValue::from(*item)).collect()),
        Value::Array(v) => JsonValue::Array(v.iter().map(value_to_json).collect()),
        Value::Range(v) => JsonValue::String(format!("{v:?}")),
        Value::Geometry(v) => JsonValue::String(format!("{:?}", v.wkb)),
        Value::Composite(v) => {
            let fields: Vec<JsonValue> = v
                .fields
                .iter()
                .map(|field| {
                    field
                        .value
                        .as_ref()
                        .map(value_to_json)
                        .unwrap_or(JsonValue::Null)
                })
                .collect();
            JsonValue::Array(fields)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    use tokio::net::{TcpListener, TcpStream};

    async fn read_test_message(stream: &mut TcpStream) -> protocol::Message {
        let mut header_bytes = [0u8; protocol::HEADER_SIZE];
        stream.read_exact(&mut header_bytes).await.unwrap();
        let header = protocol::decode_header(&header_bytes).unwrap();
        let mut payload = vec![0u8; header.length as usize];
        if header.length > 0 {
            stream.read_exact(&mut payload).await.unwrap();
        }
        protocol::Message { header, payload }
    }

    async fn write_test_message(
        stream: &mut TcpStream,
        msg_type: u8,
        payload: &[u8],
        sequence: u32,
        attachment_id: [u8; 16],
        txn_id: u64,
    ) {
        let header = protocol::MessageHeader {
            msg_type,
            flags: 0,
            length: payload.len() as u32,
            sequence,
            attachment_id,
            txn_id,
        };
        let encoded = protocol::encode_message(&header, payload);
        stream.write_all(&encoded).await.unwrap();
    }

    fn test_auth_ok_payload(session_id: [u8; 16]) -> Vec<u8> {
        let mut payload = Vec::with_capacity(20);
        payload.extend_from_slice(&session_id);
        payload.extend_from_slice(&0u32.to_le_bytes());
        payload
    }

    fn test_ready_payload(status: u8, txn_id: u64) -> Vec<u8> {
        let mut payload = vec![0u8; 20];
        payload[0] = status;
        payload[4..12].copy_from_slice(&txn_id.to_le_bytes());
        payload
    }

    fn test_command_complete_payload(
        command_type: u8,
        rows_affected: u64,
        last_insert_id: u64,
        tag: &str,
    ) -> Vec<u8> {
        let mut payload = vec![0u8; 20];
        payload[0] = command_type;
        payload[4..12].copy_from_slice(&rows_affected.to_le_bytes());
        payload[12..20].copy_from_slice(&last_insert_id.to_le_bytes());
        payload.extend_from_slice(tag.as_bytes());
        payload.push(0);
        payload
    }

    fn parse_query_sql(payload: &[u8]) -> String {
        let sql_bytes = &payload[12..];
        let end = sql_bytes
            .iter()
            .position(|byte| *byte == 0)
            .unwrap_or(sql_bytes.len());
        String::from_utf8_lossy(&sql_bytes[..end]).to_string()
    }

    fn parse_execute_max_rows(payload: &[u8]) -> u32 {
        let portal_name_len = u32::from_le_bytes(payload[0..4].try_into().unwrap()) as usize;
        let offset = 4 + portal_name_len;
        u32::from_le_bytes(payload[offset..offset + 4].try_into().unwrap())
    }

    #[test]
    fn preflight_connect_requires_manager_auth_token() {
        let mut cfg = Config::default();
        cfg.user = "tester".to_string();
        cfg.database = "db".to_string();
        cfg.front_door_mode = "manager_proxy".to_string();
        let mut client = Client::new(cfg);
        let err = client.preflight_connect().unwrap_err();
        assert_eq!(err.kind, ErrorKind::Connection);
        assert_eq!(
            err.message,
            "manager_proxy mode requires manager_auth_token"
        );
    }

    #[test]
    fn preflight_connect_allows_binary_transfer_false_and_zstd() {
        let mut cfg = Config::default();
        cfg.user = "tester".to_string();
        cfg.database = "db".to_string();
        cfg.binary_transfer = false;
        cfg.compression = "zstd".to_string();
        let mut client = Client::new(cfg);
        let params = client.preflight_connect().unwrap();
        assert_eq!(client.config.compression, "zstd");
        assert!(!client.config.binary_transfer);
        assert_eq!(params.get("database").map(String::as_str), Some("db"));
    }

    #[test]
    fn preflight_connect_rejects_unknown_compression() {
        let mut cfg = Config::default();
        cfg.user = "tester".to_string();
        cfg.database = "db".to_string();
        cfg.compression = "gzip".to_string();
        let mut client = Client::new(cfg);
        let err = client.preflight_connect().unwrap_err();
        assert_eq!(err.kind, ErrorKind::NotSupported);
        assert_eq!(err.sqlstate.as_deref(), Some("0A000"));
    }

    #[test]
    fn build_startup_params_includes_auth_plugin_selection() {
        let mut cfg = Config::default();
        cfg.user = "tester".to_string();
        cfg.database = "db".to_string();
        cfg.connect_client_flags = 257;
        cfg.auth_method_id = "scratchbird.auth.password".to_string();
        cfg.auth_method_payload = "opaque".to_string();
        cfg.auth_payload_json = "{\"tenant\":\"alpha\"}".to_string();
        cfg.auth_payload_b64 = "dGVzdA==".to_string();
        cfg.auth_provider_profile = "default".to_string();
        cfg.auth_required_methods = "SCRAM_SHA_256,TOKEN".to_string();
        cfg.auth_forbidden_methods = "MD5".to_string();
        cfg.auth_require_channel_binding = true;
        cfg.workload_identity_token = "jwt-token".to_string();
        cfg.proxy_principal_assertion = "signed-assertion".to_string();
        let client = Client::new(cfg);
        let params = client.build_startup_params().unwrap();

        assert_eq!(params.get("database").map(String::as_str), Some("db"));
        assert_eq!(params.get("user").map(String::as_str), Some("tester"));
        assert_eq!(params.get("client_flags").map(String::as_str), Some("257"));
        assert_eq!(
            params
                .get(protocol::AUTH_PARAM_METHOD_ID)
                .map(String::as_str),
            Some("scratchbird.auth.password")
        );
        assert_eq!(
            params
                .get(protocol::AUTH_PARAM_METHOD_PAYLOAD)
                .map(String::as_str),
            Some("opaque")
        );
        assert_eq!(
            params
                .get(protocol::AUTH_PARAM_PAYLOAD_JSON)
                .map(String::as_str),
            Some("{\"tenant\":\"alpha\"}")
        );
        assert_eq!(
            params
                .get(protocol::AUTH_PARAM_PAYLOAD_B64)
                .map(String::as_str),
            Some("dGVzdA==")
        );
        assert_eq!(
            params
                .get(protocol::AUTH_PARAM_PROVIDER_PROFILE)
                .map(String::as_str),
            Some("default")
        );
        assert_eq!(
            params
                .get(protocol::AUTH_PARAM_REQUIRED_METHODS)
                .map(String::as_str),
            Some("SCRAM_SHA_256,TOKEN")
        );
        assert_eq!(
            params
                .get(protocol::AUTH_PARAM_FORBIDDEN_METHODS)
                .map(String::as_str),
            Some("MD5")
        );
        assert_eq!(
            params
                .get(protocol::AUTH_PARAM_REQUIRE_CHANNEL_BINDING)
                .map(String::as_str),
            Some("1")
        );
        assert_eq!(
            params
                .get(protocol::AUTH_PARAM_WORKLOAD_IDENTITY_TOKEN)
                .map(String::as_str),
            Some("jwt-token")
        );
        assert_eq!(
            params
                .get(protocol::AUTH_PARAM_PROXY_PRINCIPAL_ASSERTION)
                .map(String::as_str),
            Some("signed-assertion")
        );
    }

    #[test]
    fn build_startup_params_rejects_invalid_auth_method_namespace() {
        let mut cfg = Config::default();
        cfg.user = "tester".to_string();
        cfg.database = "db".to_string();
        cfg.auth_method_id = "custom.invalid".to_string();
        let client = Client::new(cfg);
        let err = client.build_startup_params().unwrap_err();
        assert_eq!(err.kind, ErrorKind::Auth);
        assert_eq!(err.message, "invalid auth_method_id namespace");
    }

    #[test]
    fn resolve_token_auth_payload_prefers_auth_token_then_b64_then_json() {
        let mut cfg = Config::default();
        cfg.user = "tester".to_string();
        cfg.database = "db".to_string();
        cfg.auth_token = "bearer-token".to_string();
        cfg.auth_payload_json = "{\"token\":\"json\"}".to_string();
        cfg.auth_payload_b64 = "YWJj".to_string();
        let client = Client::new(cfg);
        let payload = client.resolve_token_auth_payload().unwrap();
        assert_eq!(payload, b"bearer-token");

        let mut cfg = Config::default();
        cfg.user = "tester".to_string();
        cfg.database = "db".to_string();
        cfg.auth_payload_json = "{\"token\":\"json\"}".to_string();
        cfg.auth_payload_b64 = "YWJj".to_string();
        let client = Client::new(cfg);
        let payload = client.resolve_token_auth_payload().unwrap();
        assert_eq!(payload, b"abc");

        let mut cfg = Config::default();
        cfg.user = "tester".to_string();
        cfg.database = "db".to_string();
        cfg.auth_payload_json = "{\"token\":\"json\"}".to_string();
        let client = Client::new(cfg);
        let payload = client.resolve_token_auth_payload().unwrap();
        assert_eq!(payload, b"{\"token\":\"json\"}");
    }

    #[test]
    fn resolve_token_auth_payload_accepts_workload_or_proxy_payloads() {
        let mut cfg = Config::default();
        cfg.user = "tester".to_string();
        cfg.database = "db".to_string();
        cfg.workload_identity_token = "jwt-token".to_string();
        let client = Client::new(cfg);
        let payload = client.resolve_token_auth_payload().unwrap();
        assert_eq!(payload, b"jwt-token");

        let mut cfg = Config::default();
        cfg.user = "tester".to_string();
        cfg.database = "db".to_string();
        cfg.proxy_principal_assertion = "signed-assertion".to_string();
        let client = Client::new(cfg);
        let payload = client.resolve_token_auth_payload().unwrap();
        assert_eq!(payload, b"signed-assertion");
    }

    #[test]
    fn native_sql_rewrites_named_placeholders() {
        let client = Client::new(Config::default());
        let mut params = HashMap::new();
        params.insert("a".to_string(), Param::from(1_i32));
        params.insert("b".to_string(), Param::from(2_i32));

        let sql = client
            .native_sql("SELECT :a, @b", Params::Named(params))
            .unwrap();

        assert_eq!(sql, "SELECT $1, $2");
    }

    #[test]
    fn native_callable_sql_rewrites_escape_call_syntax() {
        let client = Client::new(Config::default());
        let sql = client
            .native_callable_sql(
                "{ ? = call abs(?) }",
                Params::Positional(vec![Param::from(-4_i32)]),
            )
            .unwrap();
        assert_eq!(sql, "select abs($1) as return_value");
    }

    #[tokio::test]
    async fn execute_batch_rejects_empty_batch_args() {
        let mut client = Client::new(Config::default());
        let err = client
            .execute_batch("SELECT 1", Vec::new())
            .await
            .unwrap_err();
        assert_eq!(err.kind, ErrorKind::Syntax);
        assert_eq!(err.sqlstate.as_deref(), Some("07001"));
        assert_eq!(err.message, "batch arguments are required");
    }

    #[test]
    fn transaction_state_guards_enforce_begin_commit_rules() {
        let mut client = Client::new(Config::default());

        let err = client.ensure_transaction_active("commit").unwrap_err();
        assert_eq!(err.kind, ErrorKind::Transaction);
        assert_eq!(err.sqlstate.as_deref(), Some("25000"));
        assert_eq!(err.message, "cannot commit without an active transaction");

        client.apply_runtime_txn_id(42);
        let err = client.ensure_no_active_transaction().unwrap_err();
        assert_eq!(err.kind, ErrorKind::Transaction);
        assert_eq!(err.sqlstate.as_deref(), Some("25000"));
        assert_eq!(err.message, "transaction already active");
    }

    #[test]
    fn runtime_ready_can_keep_fresh_native_boundary_active_with_zero_txn_id() {
        let mut client = Client::new(Config::default());
        client.apply_runtime_ready_state(b'T', 0);
        assert!(client.has_active_transaction());
        assert_eq!(client.txn_id, 0);
    }

    #[tokio::test]
    async fn begin_restarts_implicit_boundary_and_rejects_nested_begin() {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();

        let server = tokio::spawn(async move {
            let (mut stream, _) = listener.accept().await.unwrap();
            let startup = read_test_message(&mut stream).await;
            assert_eq!(startup.header.msg_type, protocol::MSG_STARTUP);

            let attachment = [0x33_u8; 16];
            write_test_message(
                &mut stream,
                protocol::MSG_AUTH_OK,
                &test_auth_ok_payload(attachment),
                1,
                attachment,
                0,
            )
            .await;
            write_test_message(
                &mut stream,
                protocol::MSG_READY,
                &test_ready_payload(b'T', 40),
                2,
                attachment,
                40,
            )
            .await;

            let begin = read_test_message(&mut stream).await;
            assert_eq!(begin.header.msg_type, protocol::MSG_TXN_BEGIN);
            write_test_message(
                &mut stream,
                protocol::MSG_READY,
                &test_ready_payload(b'T', 41),
                3,
                attachment,
                41,
            )
            .await;
        });

        let mut cfg = Config::default();
        cfg.host = addr.ip().to_string();
        cfg.port = addr.port();
        cfg.user = "tester".to_string();
        cfg.database = "db".to_string();
        cfg.sslmode = "disable".to_string();

        let mut client = Client::new(cfg);
        client.connect().await.unwrap();
        client
            .begin_transaction(None)
            .await
            .expect("restart implicit boundary");
        assert!(client.explicit_transaction);
        assert!(client.has_active_transaction());
        assert_eq!(client.txn_id, 41);

        let err = client.begin_transaction(None).await.unwrap_err();
        assert_eq!(err.kind, ErrorKind::Transaction);
        assert_eq!(err.sqlstate.as_deref(), Some("25000"));
        assert_eq!(err.message, "transaction already active");

        server.await.unwrap();
    }

    #[tokio::test]
    async fn begin_allows_non_default_restart_options() {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();

        let server = tokio::spawn(async move {
            let (mut stream, _) = listener.accept().await.unwrap();
            let startup = read_test_message(&mut stream).await;
            assert_eq!(startup.header.msg_type, protocol::MSG_STARTUP);

            let attachment = [0x34_u8; 16];
            write_test_message(
                &mut stream,
                protocol::MSG_AUTH_OK,
                &test_auth_ok_payload(attachment),
                1,
                attachment,
                0,
            )
            .await;
            write_test_message(
                &mut stream,
                protocol::MSG_READY,
                &test_ready_payload(b'T', 40),
                2,
                attachment,
                40,
            )
            .await;

            let begin = read_test_message(&mut stream).await;
            assert_eq!(begin.header.msg_type, protocol::MSG_TXN_BEGIN);
            write_test_message(
                &mut stream,
                protocol::MSG_READY,
                &test_ready_payload(b'T', 42),
                3,
                attachment,
                42,
            )
            .await;
        });

        let mut cfg = Config::default();
        cfg.host = addr.ip().to_string();
        cfg.port = addr.port();
        cfg.user = "tester".to_string();
        cfg.database = "db".to_string();
        cfg.sslmode = "disable".to_string();

        let mut client = Client::new(cfg);
        client.connect().await.unwrap();
        client
            .begin_transaction(Some(TxnBeginOptions {
                read_committed_mode: Some(protocol::READ_COMMITTED_MODE_READ_CONSISTENCY),
                ..TxnBeginOptions::default()
            }))
            .await
            .expect("restart with non-default options");
        assert!(client.explicit_transaction);
        assert_eq!(client.txn_id, 42);

        server.await.unwrap();
    }

    #[tokio::test]
    async fn prepared_transaction_helpers_emit_canonical_control_sql() {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();

        let server = tokio::spawn(async move {
            let (mut stream, _) = listener.accept().await.unwrap();
            let startup = read_test_message(&mut stream).await;
            assert_eq!(startup.header.msg_type, protocol::MSG_STARTUP);

            let attachment = [0x22_u8; 16];
            write_test_message(
                &mut stream,
                protocol::MSG_AUTH_OK,
                &test_auth_ok_payload(attachment),
                1,
                attachment,
                0,
            )
            .await;
            write_test_message(
                &mut stream,
                protocol::MSG_READY,
                &test_ready_payload(0, 0),
                2,
                attachment,
                0,
            )
            .await;

            for (sequence, expected_sql) in [
                (3_u32, "PREPARE TRANSACTION 'gid-1'"),
                (4_u32, "COMMIT PREPARED 'gid-1'"),
                (5_u32, "ROLLBACK PREPARED 'gid''2'"),
            ] {
                let msg = read_test_message(&mut stream).await;
                assert_eq!(msg.header.msg_type, protocol::MSG_QUERY);
                assert_eq!(parse_query_sql(&msg.payload), expected_sql);
                write_test_message(
                    &mut stream,
                    protocol::MSG_COMMAND_COMPLETE,
                    &test_command_complete_payload(0, 0, 0, "OK"),
                    sequence,
                    attachment,
                    0,
                )
                .await;
                write_test_message(
                    &mut stream,
                    protocol::MSG_READY,
                    &test_ready_payload(0, 0),
                    sequence + 1,
                    attachment,
                    0,
                )
                .await;
            }
        });

        let mut cfg = Config::default();
        cfg.host = addr.ip().to_string();
        cfg.port = addr.port();
        cfg.user = "tester".to_string();
        cfg.database = "db".to_string();
        cfg.sslmode = "disable".to_string();

        let mut client = Client::new(cfg);
        client.connect().await.unwrap();

        assert!(client.supports_prepared_transactions());
        client.prepare_transaction("gid-1").await.unwrap();
        client.commit_prepared("gid-1").await.unwrap();
        client.rollback_prepared("gid'2").await.unwrap();

        server.await.unwrap();
    }

    #[test]
    fn build_prepared_transaction_sql_rejects_empty_gid() {
        let err = Client::build_prepared_transaction_sql("PREPARE TRANSACTION", "   ")
            .expect_err("empty gid should be rejected");
        assert_eq!(err.kind, ErrorKind::Syntax);
        assert_eq!(err.sqlstate.as_deref(), Some("42601"));
        assert_eq!(err.message, "global transaction id is required");
    }

    #[tokio::test]
    async fn dormant_helpers_fail_closed_and_capabilities_stay_explicit() {
        let mut client = Client::new(Config::default());

        assert!(!client.supports_dormant_reattach());

        let detach_err = client.detach_to_dormant().await.unwrap_err();
        assert_eq!(detach_err.kind, ErrorKind::NotSupported);
        assert_eq!(detach_err.sqlstate.as_deref(), Some("0A000"));

        let reattach_err = client
            .reattach_dormant("dormant-1", Some("token-1"))
            .await
            .unwrap_err();
        assert_eq!(reattach_err.kind, ErrorKind::NotSupported);
        assert_eq!(reattach_err.sqlstate.as_deref(), Some("0A000"));
    }

    #[tokio::test]
    async fn resume_suspended_portal_requires_explicit_pending_state() {
        let mut client = Client::new(Config::default());
        let err = client.resume_suspended_portal(2).await.unwrap_err();
        assert_eq!(err.kind, ErrorKind::Transaction);
        assert_eq!(err.sqlstate.as_deref(), Some("55000"));
        assert_eq!(
            err.message,
            "portal resume requires explicit suspended state"
        );
    }

    #[tokio::test]
    async fn query_stream_resumes_only_after_explicit_suspended_state() {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();

        let server = tokio::spawn(async move {
            let (mut stream, _) = listener.accept().await.unwrap();
            let startup = read_test_message(&mut stream).await;
            assert_eq!(startup.header.msg_type, protocol::MSG_STARTUP);

            let attachment = [0x33_u8; 16];
            write_test_message(
                &mut stream,
                protocol::MSG_AUTH_OK,
                &test_auth_ok_payload(attachment),
                1,
                attachment,
                0,
            )
            .await;
            write_test_message(
                &mut stream,
                protocol::MSG_READY,
                &test_ready_payload(0, 0),
                2,
                attachment,
                0,
            )
            .await;

            let query = read_test_message(&mut stream).await;
            assert_eq!(query.header.msg_type, protocol::MSG_QUERY);
            assert_eq!(parse_query_sql(&query.payload), "SELECT 1");

            write_test_message(
                &mut stream,
                protocol::MSG_PORTAL_SUSPENDED,
                &[],
                3,
                attachment,
                0,
            )
            .await;

            let execute = read_test_message(&mut stream).await;
            assert_eq!(execute.header.msg_type, protocol::MSG_EXECUTE);
            assert_eq!(parse_execute_max_rows(&execute.payload), 2);

            write_test_message(
                &mut stream,
                protocol::MSG_READY,
                &test_ready_payload(0, 77),
                4,
                attachment,
                77,
            )
            .await;
        });

        let mut cfg = Config::default();
        cfg.host = addr.ip().to_string();
        cfg.port = addr.port();
        cfg.user = "tester".to_string();
        cfg.database = "db".to_string();
        cfg.sslmode = "disable".to_string();
        cfg.fetch_size = 2;

        let mut client = Client::new(cfg);
        client.connect().await.unwrap();
        {
            let mut stream = client.query_stream("SELECT 1").await.unwrap();
            assert!(stream.next_row().await.unwrap().is_none());
        }
        assert_eq!(client.txn_id, 0);
        assert!(!client.runtime_txn_active);
        assert!(!client.portal_resume_pending);

        server.await.unwrap();
    }

    #[tokio::test]
    async fn connect_clears_abandoned_session_state_before_replacement_handshake() {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();

        let server = tokio::spawn(async move {
            let (mut stream, _) = listener.accept().await.unwrap();
            let startup = read_test_message(&mut stream).await;
            assert_eq!(startup.header.msg_type, protocol::MSG_STARTUP);
            assert_eq!(startup.header.sequence, 0);

            let replacement_attachment = [0x11_u8; 16];
            write_test_message(
                &mut stream,
                protocol::MSG_AUTH_OK,
                &test_auth_ok_payload(replacement_attachment),
                1,
                replacement_attachment,
                0,
            )
            .await;
            write_test_message(
                &mut stream,
                protocol::MSG_READY,
                &test_ready_payload(0, 0),
                2,
                replacement_attachment,
                0,
            )
            .await;
        });

        let mut cfg = Config::default();
        cfg.host = addr.ip().to_string();
        cfg.port = addr.port();
        cfg.user = "tester".to_string();
        cfg.database = "db".to_string();
        cfg.sslmode = "disable".to_string();

        let mut client = Client::new(cfg);
        client.connected = true;
        client.authed = true;
        client.attachment_id = [0x7f_u8; 16];
        client.txn_id = 77;
        client.sequence = 9;
        client.last_query_sequence = 5;
        client
            .parameters
            .insert("attachment_id".to_string(), "stale".to_string());
        client.last_plan = Some(protocol::QueryPlan {
            format: 1,
            planning_time_us: 1,
            estimated_rows: 1,
            estimated_cost: 1,
            plan: b"stale".to_vec(),
        });
        client.last_sblr = Some(protocol::SblrCompiled {
            hash: 9,
            version: 1,
            bytecode: vec![0x42],
        });

        client.connect().await.unwrap();
        server.await.unwrap();

        assert!(client.connected);
        assert!(client.authed);
        assert_eq!(client.attachment_id, [0x11_u8; 16]);
        assert_eq!(client.txn_id, 0);
        assert_eq!(client.sequence, 1);
        assert_eq!(client.last_query_sequence, 0);
        assert!(client.parameters.is_empty());
        assert!(client.last_plan.is_none());
        assert!(client.last_sblr.is_none());
    }

    #[tokio::test]
    async fn set_autocommit_requires_connected_client() {
        let mut client = Client::new(Config::default());
        let err = client.set_autocommit(false).await.unwrap_err();
        assert_eq!(err.kind, ErrorKind::Connection);
        assert_eq!(err.message, "client is not connected");
    }

    #[test]
    fn savepoint_name_validation_rejects_blank() {
        let err = Client::validate_savepoint_name("   ").unwrap_err();
        assert_eq!(err.kind, ErrorKind::Syntax);
        assert_eq!(err.sqlstate.as_deref(), Some("42601"));
        assert_eq!(err.message, "savepoint name is required");

        let name = Client::validate_savepoint_name("  sp_a  ").unwrap();
        assert_eq!(name, "sp_a");
    }

    #[test]
    fn metadata_collection_name_rejects_unknown_collection() {
        let err = Client::metadata_collection_name("not_a_collection").unwrap_err();
        assert_eq!(err.kind, ErrorKind::NotSupported);
        assert_eq!(err.sqlstate.as_deref(), Some("0A000"));
    }

    #[tokio::test]
    async fn query_metadata_rejects_unknown_collection_before_connect() {
        let mut client = Client::new(Config::default());
        let err = client.query_metadata("bad_collection").await.unwrap_err();
        assert_eq!(err.kind, ErrorKind::NotSupported);
        assert_eq!(err.sqlstate.as_deref(), Some("0A000"));
    }

    #[tokio::test]
    async fn query_metadata_requires_connected_client_for_supported_collection() {
        let mut client = Client::new(Config::default());
        let err = client.query_metadata("schemas").await.unwrap_err();
        assert_eq!(err.kind, ErrorKind::Connection);
        assert_eq!(err.message, "client is not connected");
    }

    #[tokio::test]
    async fn query_metadata_with_restrictions_rejects_unknown_collection_before_connect() {
        let mut client = Client::new(Config::default());
        let restrictions = HashMap::from([("schema".to_string(), "sys".to_string())]);
        let err = client
            .query_metadata_with_restrictions("bad_collection", &restrictions)
            .await
            .unwrap_err();
        assert_eq!(err.kind, ErrorKind::NotSupported);
        assert_eq!(err.sqlstate.as_deref(), Some("0A000"));
    }

    #[test]
    fn apply_metadata_restrictions_filters_rows_by_aliases() {
        let result = QueryResult {
            columns: vec![
                Column {
                    name: "TABLE_SCHEM".to_string(),
                    type_oid: 0,
                    type_modifier: 0,
                    format: 0,
                    nullable: false,
                },
                Column {
                    name: "TABLE_NAME".to_string(),
                    type_oid: 0,
                    type_modifier: 0,
                    format: 0,
                    nullable: false,
                },
            ],
            rows: vec![
                vec![
                    Value::String("users.alice".to_string()),
                    Value::String("accounts".to_string()),
                ],
                vec![
                    Value::String("users.bob".to_string()),
                    Value::String("accounts".to_string()),
                ],
                vec![
                    Value::String("users.alice".to_string()),
                    Value::String("sessions".to_string()),
                ],
            ],
            row_count: 3,
            command_tag: "SELECT".to_string(),
        };

        let restrictions = HashMap::from([
            ("schema".to_string(), "USERS.ALICE".to_string()),
            ("table".to_string(), "accounts".to_string()),
        ]);

        let filtered = apply_metadata_restrictions(result, &restrictions, None);
        assert_eq!(filtered.row_count, 1);
        assert_eq!(filtered.rows.len(), 1);
        match &filtered.rows[0][0] {
            Value::String(value) => assert_eq!(value, "users.alice"),
            other => panic!("unexpected schema value: {other:?}"),
        }
        match &filtered.rows[0][1] {
            Value::String(value) => assert_eq!(value, "accounts"),
            other => panic!("unexpected table value: {other:?}"),
        }
    }

    #[test]
    fn apply_metadata_restrictions_supports_null_and_ignores_unknown_restrictions() {
        let result = QueryResult {
            columns: vec![
                Column {
                    name: "REMARKS".to_string(),
                    type_oid: 0,
                    type_modifier: 0,
                    format: 0,
                    nullable: true,
                },
                Column {
                    name: "TABLE_NAME".to_string(),
                    type_oid: 0,
                    type_modifier: 0,
                    format: 0,
                    nullable: false,
                },
            ],
            rows: vec![
                vec![Value::Null, Value::String("t1".to_string())],
                vec![
                    Value::String("documented".to_string()),
                    Value::String("t2".to_string()),
                ],
            ],
            row_count: 2,
            command_tag: "SELECT".to_string(),
        };

        let restrictions = HashMap::from([
            ("remarks".to_string(), "NULL".to_string()),
            ("unknown_key".to_string(), "ignored".to_string()),
        ]);
        let filtered = apply_metadata_restrictions(result, &restrictions, None);
        assert_eq!(filtered.row_count, 1);
        assert_eq!(filtered.rows.len(), 1);
        match &filtered.rows[0][0] {
            Value::Null => {}
            other => panic!("expected null remarks, saw {other:?}"),
        }
        match &filtered.rows[0][1] {
            Value::String(value) => assert_eq!(value, "t1"),
            other => panic!("unexpected table value: {other:?}"),
        }
    }

    #[test]
    fn apply_metadata_restrictions_uses_collection_specific_allowed_keys() {
        let result = QueryResult {
            columns: vec![
                Column {
                    name: "TABLE_NAME".to_string(),
                    type_oid: 0,
                    type_modifier: 0,
                    format: 0,
                    nullable: false,
                },
                Column {
                    name: "COLUMN_NAME".to_string(),
                    type_oid: 0,
                    type_modifier: 0,
                    format: 0,
                    nullable: false,
                },
            ],
            rows: vec![
                vec![
                    Value::String("accounts".to_string()),
                    Value::String("account_id".to_string()),
                ],
                vec![
                    Value::String("sessions".to_string()),
                    Value::String("session_id".to_string()),
                ],
            ],
            row_count: 2,
            command_tag: "SELECT".to_string(),
        };

        let restrictions = HashMap::from([
            ("column".to_string(), "account_id".to_string()),
            ("table".to_string(), "accounts".to_string()),
        ]);

        let filtered = apply_metadata_restrictions(result, &restrictions, Some("tables"));
        assert_eq!(filtered.row_count, 1);
        assert_eq!(filtered.rows.len(), 1);
        match &filtered.rows[0][0] {
            Value::String(value) => assert_eq!(value, "accounts"),
            other => panic!("unexpected table value: {other:?}"),
        }
    }

    #[test]
    fn apply_metadata_restrictions_matches_any_alias_column_for_same_restriction() {
        let result = QueryResult {
            columns: vec![
                Column {
                    name: "TABLE_SCHEMA".to_string(),
                    type_oid: 0,
                    type_modifier: 0,
                    format: 0,
                    nullable: false,
                },
                Column {
                    name: "TABLE_SCHEM".to_string(),
                    type_oid: 0,
                    type_modifier: 0,
                    format: 0,
                    nullable: false,
                },
            ],
            rows: vec![
                vec![
                    Value::String("users.alice".to_string()),
                    Value::String("legacy".to_string()),
                ],
                vec![
                    Value::String("legacy".to_string()),
                    Value::String("users.alice".to_string()),
                ],
                vec![
                    Value::String("users.bob".to_string()),
                    Value::String("users.bob".to_string()),
                ],
            ],
            row_count: 3,
            command_tag: "SELECT".to_string(),
        };

        let restrictions = HashMap::from([("schema".to_string(), "users.alice".to_string())]);
        let filtered = apply_metadata_restrictions(result, &restrictions, Some("schemas"));
        assert_eq!(filtered.row_count, 2);
        assert_eq!(filtered.rows.len(), 2);
    }

    #[test]
    fn apply_metadata_restrictions_supports_routines_collection_aliases() {
        let result = QueryResult {
            columns: vec![
                Column {
                    name: "SCHEMA_NAME".to_string(),
                    type_oid: 0,
                    type_modifier: 0,
                    format: 0,
                    nullable: false,
                },
                Column {
                    name: "PROCEDURE_NAME".to_string(),
                    type_oid: 0,
                    type_modifier: 0,
                    format: 0,
                    nullable: true,
                },
                Column {
                    name: "FUNCTION_NAME".to_string(),
                    type_oid: 0,
                    type_modifier: 0,
                    format: 0,
                    nullable: true,
                },
            ],
            rows: vec![
                vec![
                    Value::String("users.alice".to_string()),
                    Value::String("sync_accounts".to_string()),
                    Value::Null,
                ],
                vec![
                    Value::String("users.alice".to_string()),
                    Value::Null,
                    Value::String("sync_profiles".to_string()),
                ],
            ],
            row_count: 2,
            command_tag: "SELECT".to_string(),
        };

        let restrictions = HashMap::from([
            ("schema".to_string(), "users.alice".to_string()),
            ("routine".to_string(), "sync_profiles".to_string()),
        ]);

        let filtered = apply_metadata_restrictions(result, &restrictions, Some("routines"));
        assert_eq!(filtered.row_count, 1);
        assert_eq!(filtered.rows.len(), 1);
        match &filtered.rows[0][2] {
            Value::String(value) => assert_eq!(value, "sync_profiles"),
            other => panic!("unexpected routine value: {other:?}"),
        }
    }

    #[test]
    fn like_match_supports_wildcards_and_escape() {
        assert!(like_match("users.alice", "users.%"));
        assert!(like_match("users_alice", "users\\_alice"));
        assert!(like_match("ab", "a_"));
        assert!(!like_match("users.alice", "admin.%"));
    }

    #[test]
    fn filter_schema_metadata_rows_by_pattern_applies_case_insensitive_match() {
        let rows = vec![
            HashMap::from([(
                "schema_name".to_string(),
                JsonValue::String("users.alice".to_string()),
            )]),
            HashMap::from([(
                "schema_name".to_string(),
                JsonValue::String("admin.root".to_string()),
            )]),
        ];
        let filtered = filter_schema_metadata_rows_by_pattern(rows, Some("USERS.%"));
        assert_eq!(filtered.len(), 1);
        assert_eq!(
            filtered[0].get("schema_name"),
            Some(&JsonValue::String("users.alice".to_string()))
        );
    }

    #[test]
    fn encode_txn_begin_options_includes_read_committed_mode() {
        let payload = encode_txn_begin_options(&TxnBeginOptions {
            read_committed_mode: Some(protocol::READ_COMMITTED_MODE_READ_CONSISTENCY),
            timeout_ms: Some(25),
            ..TxnBeginOptions::default()
        })
        .expect("encode read committed mode");
        assert_eq!(payload.len(), 16);
        let flags = u16::from_le_bytes([payload[0], payload[1]]);
        assert!(flags & protocol::TXN_FLAG_HAS_ISOLATION != 0);
        assert!(flags & protocol::TXN_FLAG_HAS_TIMEOUT != 0);
        assert!(flags & protocol::TXN_FLAG_HAS_READ_COMMITTED_MODE != 0);
        assert_eq!(payload[4], protocol::ISOLATION_READ_COMMITTED);
        assert_eq!(u32::from_le_bytes(payload[8..12].try_into().unwrap()), 25);
        assert_eq!(payload[12], protocol::READ_COMMITTED_MODE_READ_CONSISTENCY);
    }

    #[test]
    fn encode_txn_begin_options_rejects_snapshot_alias_for_read_committed_mode() {
        let err = encode_txn_begin_options(&TxnBeginOptions {
            isolation_level: Some(protocol::ISOLATION_SERIALIZABLE),
            read_committed_mode: Some(protocol::READ_COMMITTED_MODE_READ_CONSISTENCY),
            ..TxnBeginOptions::default()
        })
        .expect_err("snapshot alias should be rejected");
        assert_eq!(err.kind, ErrorKind::NotSupported);
        assert_eq!(err.sqlstate.as_deref(), Some("0A000"));
        assert!(err.message.contains("READ COMMITTED isolation alias"));
    }
}
