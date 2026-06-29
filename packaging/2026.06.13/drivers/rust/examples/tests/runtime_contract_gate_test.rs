// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::{Arc, Mutex};

use scratchbird::protocol::{self, MessageHeader};
use scratchbird::sql::Params;
use scratchbird::types::{self, Value};
use scratchbird::{Client, Config, ErrorKind};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::task::JoinHandle;

#[derive(Debug, Clone)]
struct RuntimeGateOptions {
    manager_proxy: bool,
    auth_method: u8,
    manager_auth_success: bool,
    zero_txn_ready_status: u8,
}

impl RuntimeGateOptions {
    fn direct(auth_method: u8) -> Self {
        Self {
            manager_proxy: false,
            auth_method,
            manager_auth_success: true,
            zero_txn_ready_status: 0,
        }
    }
}

#[derive(Debug, Clone, Default)]
struct RuntimeGateState {
    startup_features: u64,
    query_flags: Vec<u32>,
    auth_responses: Vec<Vec<u8>>,
    manager_frames: Vec<u8>,
    set_options: Vec<(String, String)>,
    events: Vec<String>,
}

struct RuntimeGateServer {
    addr: SocketAddr,
    state: Arc<Mutex<RuntimeGateState>>,
    handle: JoinHandle<std::result::Result<(), String>>,
}

impl RuntimeGateServer {
    async fn start(options: RuntimeGateOptions) -> Self {
        let listener = TcpListener::bind("127.0.0.1:0")
            .await
            .expect("bind runtime gate listener");
        let addr = listener
            .local_addr()
            .expect("runtime gate listener local addr");
        let state = Arc::new(Mutex::new(RuntimeGateState::default()));
        let state_for_task = Arc::clone(&state);
        let handle = tokio::spawn(async move {
            let (stream, _) = listener.accept().await.map_err(|e| e.to_string())?;
            run_runtime_gate_connection(stream, options, state_for_task).await
        });

        Self {
            addr,
            state,
            handle,
        }
    }

    fn addr(&self) -> SocketAddr {
        self.addr
    }

    fn snapshot(&self) -> RuntimeGateState {
        self.state.lock().expect("runtime gate state lock").clone()
    }

    async fn finish(self) {
        match self.handle.await {
            Ok(Ok(())) => {}
            Ok(Err(err)) => panic!("runtime gate server failed: {err}"),
            Err(err) => panic!("runtime gate task join failure: {err}"),
        }
    }
}

#[derive(Debug, Clone, Copy)]
struct RuntimeColumn {
    name: &'static str,
    oid: u32,
}

async fn run_runtime_gate_connection(
    mut stream: TcpStream,
    options: RuntimeGateOptions,
    state: Arc<Mutex<RuntimeGateState>>,
) -> std::result::Result<(), String> {
    let mut sequence = 0u32;
    let mut txn_id = 0u64;
    let attachment_id = [0x22; 16];
    let zero_txn_ready_status = options.zero_txn_ready_status;

    if options.manager_proxy {
        let continue_to_sbwp =
            handle_manager_proxy(&mut stream, &state, options.manager_auth_success).await?;
        if !continue_to_sbwp {
            return Ok(());
        }
    }

    let startup = read_protocol_message(&mut stream).await?;
    if startup.header.msg_type != protocol::MSG_STARTUP {
        return Err(format!(
            "expected startup message, got {}",
            startup.header.msg_type
        ));
    }
    if startup.payload.len() < 12 {
        return Err(format!(
            "startup payload too short: {}",
            startup.payload.len()
        ));
    }
    {
        let mut locked = state
            .lock()
            .map_err(|_| "state mutex poisoned".to_string())?;
        locked.startup_features = u64::from_le_bytes(
            startup.payload[4..12]
                .try_into()
                .map_err(|_| "invalid startup features bytes".to_string())?,
        );
    }

    if options.auth_method != protocol::AUTH_OK {
        write_protocol_message(
            &mut stream,
            &mut sequence,
            protocol::MSG_AUTH_REQUEST,
            &auth_request_payload(options.auth_method),
            attachment_id,
            txn_id,
        )
        .await?;
        let response = match read_protocol_message(&mut stream).await {
            Ok(message) => message,
            Err(err) if err.contains("early eof") => return Ok(()),
            Err(err) => return Err(err),
        };
        if response.header.msg_type != protocol::MSG_AUTH_RESPONSE {
            return Err(format!(
                "expected auth response, got {}",
                response.header.msg_type
            ));
        }
        {
            let mut locked = state
                .lock()
                .map_err(|_| "state mutex poisoned".to_string())?;
            locked.auth_responses.push(response.payload.clone());
        }

        if options.auth_method == protocol::AUTH_SCRAM_SHA256
            || options.auth_method == protocol::AUTH_SCRAM_SHA512
        {
            let client_first = String::from_utf8_lossy(&response.payload).to_string();
            let nonce = parse_scram_client_nonce(&client_first)?;
            let server_first = format!("r={}server,s=c2FsdA==,i=4096", nonce);
            write_protocol_message(
                &mut stream,
                &mut sequence,
                protocol::MSG_AUTH_CONTINUE,
                &auth_continue_payload(options.auth_method, 1, server_first.as_bytes()),
                attachment_id,
                txn_id,
            )
            .await?;
            let final_response = read_protocol_message(&mut stream).await?;
            if final_response.header.msg_type != protocol::MSG_AUTH_RESPONSE {
                return Err(format!(
                    "expected SCRAM final auth response, got {}",
                    final_response.header.msg_type
                ));
            }
            let mut locked = state
                .lock()
                .map_err(|_| "state mutex poisoned".to_string())?;
            locked.auth_responses.push(final_response.payload);
        }
    }

    write_protocol_message(
        &mut stream,
        &mut sequence,
        protocol::MSG_AUTH_OK,
        &auth_ok_payload(&[]),
        attachment_id,
        txn_id,
    )
    .await?;
    write_protocol_message(
        &mut stream,
        &mut sequence,
        protocol::MSG_READY,
        &ready_payload(
            if txn_id == 0 {
                zero_txn_ready_status
            } else {
                1
            },
            txn_id,
        ),
        attachment_id,
        txn_id,
    )
    .await?;

    loop {
        let message = read_protocol_message(&mut stream).await?;
        match message.header.msg_type {
            protocol::MSG_TXN_BEGIN => {
                txn_id = 77;
                {
                    let mut locked = state
                        .lock()
                        .map_err(|_| "state mutex poisoned".to_string())?;
                    locked.events.push("txn_begin".to_string());
                }
                write_protocol_message(
                    &mut stream,
                    &mut sequence,
                    protocol::MSG_READY,
                    &ready_payload(1, txn_id),
                    attachment_id,
                    txn_id,
                )
                .await?;
            }
            protocol::MSG_TXN_SAVEPOINT => {
                {
                    let mut locked = state
                        .lock()
                        .map_err(|_| "state mutex poisoned".to_string())?;
                    locked.events.push("txn_savepoint".to_string());
                }
                write_protocol_message(
                    &mut stream,
                    &mut sequence,
                    protocol::MSG_READY,
                    &ready_payload(1, txn_id),
                    attachment_id,
                    txn_id,
                )
                .await?;
            }
            protocol::MSG_TXN_RELEASE => {
                {
                    let mut locked = state
                        .lock()
                        .map_err(|_| "state mutex poisoned".to_string())?;
                    locked.events.push("txn_release".to_string());
                }
                write_protocol_message(
                    &mut stream,
                    &mut sequence,
                    protocol::MSG_READY,
                    &ready_payload(1, txn_id),
                    attachment_id,
                    txn_id,
                )
                .await?;
            }
            protocol::MSG_TXN_ROLLBACK_TO => {
                {
                    let mut locked = state
                        .lock()
                        .map_err(|_| "state mutex poisoned".to_string())?;
                    locked.events.push("txn_rollback_to".to_string());
                }
                write_protocol_message(
                    &mut stream,
                    &mut sequence,
                    protocol::MSG_READY,
                    &ready_payload(1, txn_id),
                    attachment_id,
                    txn_id,
                )
                .await?;
            }
            protocol::MSG_TXN_COMMIT => {
                txn_id = 0;
                {
                    let mut locked = state
                        .lock()
                        .map_err(|_| "state mutex poisoned".to_string())?;
                    locked.events.push("txn_commit".to_string());
                }
                write_protocol_message(
                    &mut stream,
                    &mut sequence,
                    protocol::MSG_READY,
                    &ready_payload(
                        if txn_id == 0 {
                            zero_txn_ready_status
                        } else {
                            1
                        },
                        txn_id,
                    ),
                    attachment_id,
                    txn_id,
                )
                .await?;
            }
            protocol::MSG_TXN_ROLLBACK => {
                txn_id = 0;
                {
                    let mut locked = state
                        .lock()
                        .map_err(|_| "state mutex poisoned".to_string())?;
                    locked.events.push("txn_rollback".to_string());
                }
                write_protocol_message(
                    &mut stream,
                    &mut sequence,
                    protocol::MSG_READY,
                    &ready_payload(
                        if txn_id == 0 {
                            zero_txn_ready_status
                        } else {
                            1
                        },
                        txn_id,
                    ),
                    attachment_id,
                    txn_id,
                )
                .await?;
            }
            protocol::MSG_SET_OPTION => {
                let (name, value) = parse_set_option_payload(&message.payload)?;
                {
                    let mut locked = state
                        .lock()
                        .map_err(|_| "state mutex poisoned".to_string())?;
                    locked.set_options.push((name.clone(), value.clone()));
                    locked.events.push(format!("set_option:{name}={value}"));
                }
                write_protocol_message(
                    &mut stream,
                    &mut sequence,
                    protocol::MSG_READY,
                    &ready_payload(
                        if txn_id == 0 {
                            zero_txn_ready_status
                        } else {
                            1
                        },
                        txn_id,
                    ),
                    attachment_id,
                    txn_id,
                )
                .await?;
            }
            protocol::MSG_PING => {
                write_protocol_message(
                    &mut stream,
                    &mut sequence,
                    protocol::MSG_PONG,
                    &[],
                    attachment_id,
                    txn_id,
                )
                .await?;
            }
            protocol::MSG_QUERY => {
                let (flags, sql) = parse_query_payload(&message.payload)?;
                {
                    let mut locked = state
                        .lock()
                        .map_err(|_| "state mutex poisoned".to_string())?;
                    locked.query_flags.push(flags);
                }
                handle_query(&mut stream, &mut sequence, attachment_id, txn_id, &sql).await?;
                write_protocol_message(
                    &mut stream,
                    &mut sequence,
                    protocol::MSG_READY,
                    &ready_payload(0, txn_id),
                    attachment_id,
                    txn_id,
                )
                .await?;
            }
            protocol::MSG_TERMINATE => return Ok(()),
            other => return Err(format!("unexpected message type {other}")),
        }
    }
}

async fn handle_manager_proxy(
    stream: &mut TcpStream,
    state: &Arc<Mutex<RuntimeGateState>>,
    auth_success: bool,
) -> std::result::Result<bool, String> {
    let (msg_type, _) = read_manager_frame(stream).await?;
    if msg_type != MCP_MSG_HELLO {
        return Err(format!("expected MCP hello, got {msg_type}"));
    }
    {
        let mut locked = state
            .lock()
            .map_err(|_| "state mutex poisoned".to_string())?;
        locked.manager_frames.push(msg_type);
    }
    write_manager_frame(stream, MCP_MSG_STATUS_RESPONSE, &[0]).await?;

    let (msg_type, payload) = read_manager_frame(stream).await?;
    if msg_type != MCP_MSG_AUTH_START {
        return Err(format!("expected MCP auth start, got {msg_type}"));
    }
    {
        let mut locked = state
            .lock()
            .map_err(|_| "state mutex poisoned".to_string())?;
        locked.manager_frames.push(msg_type);
    }
    let token_len = extract_manager_auth_start_token_len(&payload);
    if token_len == 0 {
        write_manager_frame(stream, MCP_MSG_AUTH_CHALLENGE, &[0]).await?;
        let (msg_type, _) = match read_manager_frame(stream).await {
            Ok(frame) => frame,
            Err(err) if err.contains("early eof") => return Ok(false),
            Err(err) => return Err(err),
        };
        if msg_type != MCP_MSG_AUTH_CONTINUE {
            return Err(format!("expected MCP auth continue, got {msg_type}"));
        }
        let mut locked = state
            .lock()
            .map_err(|_| "state mutex poisoned".to_string())?;
        locked.manager_frames.push(msg_type);
    }

    if !auth_success {
        write_manager_frame(
            stream,
            MCP_MSG_AUTH_RESPONSE,
            &manager_auth_response_failure("invalid token"),
        )
        .await?;
        return Ok(false);
    }

    write_manager_frame(
        stream,
        MCP_MSG_AUTH_RESPONSE,
        &manager_auth_response_success(),
    )
    .await?;
    let (msg_type, _) = read_manager_frame(stream).await?;
    if msg_type != MCP_MSG_DB_CONNECT {
        return Err(format!("expected MCP db connect, got {msg_type}"));
    }
    {
        let mut locked = state
            .lock()
            .map_err(|_| "state mutex poisoned".to_string())?;
        locked.manager_frames.push(msg_type);
    }
    write_manager_frame(
        stream,
        MCP_MSG_CONNECT_RESPONSE,
        &manager_connect_response_success(),
    )
    .await?;
    Ok(true)
}

async fn handle_query(
    stream: &mut TcpStream,
    sequence: &mut u32,
    attachment_id: [u8; 16],
    txn_id: u64,
    sql: &str,
) -> std::result::Result<(), String> {
    let normalized = sql.trim().to_ascii_lowercase();
    if normalized == "select abs(-3) as return_value" || normalized == "select abs(-3)" {
        return write_result_set(
            stream,
            sequence,
            attachment_id,
            txn_id,
            &[RuntimeColumn {
                name: "abs",
                oid: types::OID_INT4,
            }],
            &[&["3"]],
            "SELECT 1",
        )
        .await;
    }

    if normalized.starts_with("insert into generated_key_fixture") {
        write_protocol_message(
            stream,
            sequence,
            protocol::MSG_COMMAND_COMPLETE,
            &command_complete_payload(1, 41, "INSERT 0 1"),
            attachment_id,
            txn_id,
        )
        .await?;
        return Ok(());
    }

    if normalized == "select 1; select 2" {
        write_result_set(
            stream,
            sequence,
            attachment_id,
            txn_id,
            &[RuntimeColumn {
                name: "first_value",
                oid: types::OID_INT4,
            }],
            &[&["1"]],
            "SELECT 1",
        )
        .await?;
        write_result_set(
            stream,
            sequence,
            attachment_id,
            txn_id,
            &[RuntimeColumn {
                name: "second_value",
                oid: types::OID_INT4,
            }],
            &[&["2"]],
            "SELECT 1",
        )
        .await?;
        return Ok(());
    }

    if normalized == "select 1" {
        return write_result_set(
            stream,
            sequence,
            attachment_id,
            txn_id,
            &[RuntimeColumn {
                name: "value",
                oid: types::OID_INT4,
            }],
            &[&["1"]],
            "SELECT 1",
        )
        .await;
    }

    if normalized == "select 2" {
        return write_result_set(
            stream,
            sequence,
            attachment_id,
            txn_id,
            &[RuntimeColumn {
                name: "value",
                oid: types::OID_INT4,
            }],
            &[&["2"]],
            "SELECT 1",
        )
        .await;
    }

    if normalized.contains("from sys.schemas") && normalized.contains("catalog_name") {
        return write_result_set(
            stream,
            sequence,
            attachment_id,
            txn_id,
            &[
                RuntimeColumn {
                    name: "catalog_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "catalog_name",
                    oid: types::OID_TEXT,
                },
            ],
            &[&["1", "users.alice"]],
            "SELECT 1",
        )
        .await;
    }
    if normalized.contains("from sys.schemas") {
        return write_result_set(
            stream,
            sequence,
            attachment_id,
            txn_id,
            &[
                RuntimeColumn {
                    name: "schema_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "schema_name",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "owner_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "default_tablespace_id",
                    oid: types::OID_TEXT,
                },
            ],
            &[&["1", "users.alice", "2", "0"]],
            "SELECT 1",
        )
        .await;
    }
    if normalized.contains("from sys.tables") && normalized.contains("privilege_type") {
        return write_result_set(
            stream,
            sequence,
            attachment_id,
            txn_id,
            &[
                RuntimeColumn {
                    name: "table_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "table_name",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "grantor_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "grantee_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "privilege_type",
                    oid: types::OID_TEXT,
                },
            ],
            &[&["10", "accounts", "2", "2", "ALL"]],
            "SELECT 1",
        )
        .await;
    }
    if normalized.contains("from sys.tables") {
        return write_result_set(
            stream,
            sequence,
            attachment_id,
            txn_id,
            &[
                RuntimeColumn {
                    name: "table_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "schema_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "table_name",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "table_type",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "owner_id",
                    oid: types::OID_TEXT,
                },
            ],
            &[&["10", "1", "accounts", "TABLE", "2"]],
            "SELECT 1",
        )
        .await;
    }
    if normalized.contains("from sys.columns") && normalized.contains("distinct data_type_id") {
        return write_result_set(
            stream,
            sequence,
            attachment_id,
            txn_id,
            &[
                RuntimeColumn {
                    name: "data_type_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "data_type_name",
                    oid: types::OID_TEXT,
                },
            ],
            &[&["23", "INT4"]],
            "SELECT 1",
        )
        .await;
    }
    if normalized.contains("from sys.columns") && normalized.contains("privilege_type") {
        return write_result_set(
            stream,
            sequence,
            attachment_id,
            txn_id,
            &[
                RuntimeColumn {
                    name: "table_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "column_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "column_name",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "privilege_type",
                    oid: types::OID_TEXT,
                },
            ],
            &[&["10", "1", "account_id", "ALL"]],
            "SELECT 1",
        )
        .await;
    }
    if normalized.contains("from sys.columns") {
        return write_result_set(
            stream,
            sequence,
            attachment_id,
            txn_id,
            &[
                RuntimeColumn {
                    name: "column_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "table_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "column_name",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "data_type_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "data_type_name",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "ordinal_position",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "is_nullable",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "default_value",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "domain_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "collation_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "charset_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "is_identity",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "is_generated",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "generation_expression",
                    oid: types::OID_TEXT,
                },
            ],
            &[&[
                "1",
                "10",
                "account_id",
                "23",
                "INT4",
                "1",
                "false",
                "",
                "",
                "",
                "",
                "false",
                "false",
                "",
            ]],
            "SELECT 1",
        )
        .await;
    }
    if normalized.contains("from sys.index_columns") {
        return write_result_set(
            stream,
            sequence,
            attachment_id,
            txn_id,
            &[
                RuntimeColumn {
                    name: "index_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "column_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "column_name",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "ordinal_position",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "is_included",
                    oid: types::OID_TEXT,
                },
            ],
            &[&["5", "1", "account_id", "1", "false"]],
            "SELECT 1",
        )
        .await;
    }
    if normalized.contains("from sys.indexes") {
        return write_result_set(
            stream,
            sequence,
            attachment_id,
            txn_id,
            &[
                RuntimeColumn {
                    name: "index_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "table_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "index_name",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "index_type",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "is_unique",
                    oid: types::OID_TEXT,
                },
            ],
            &[&["5", "10", "idx_accounts_id", "btree", "true"]],
            "SELECT 1",
        )
        .await;
    }
    if normalized.contains("from sys.constraints")
        && (normalized.contains("foreign key") || normalized.contains("'foreign'"))
    {
        return write_result_set(
            stream,
            sequence,
            attachment_id,
            txn_id,
            &[
                RuntimeColumn {
                    name: "constraint_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "table_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "constraint_name",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "constraint_type",
                    oid: types::OID_TEXT,
                },
            ],
            &[&["8", "10", "fk_accounts_owner", "FOREIGN KEY"]],
            "SELECT 1",
        )
        .await;
    }
    if normalized.contains("from sys.constraints")
        && (normalized.contains("primary key") || normalized.contains("'primary'"))
    {
        return write_result_set(
            stream,
            sequence,
            attachment_id,
            txn_id,
            &[
                RuntimeColumn {
                    name: "constraint_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "table_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "constraint_name",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "constraint_type",
                    oid: types::OID_TEXT,
                },
            ],
            &[&["7", "10", "pk_accounts", "PRIMARY KEY"]],
            "SELECT 1",
        )
        .await;
    }
    if normalized.contains("from sys.constraints") {
        return write_result_set(
            stream,
            sequence,
            attachment_id,
            txn_id,
            &[
                RuntimeColumn {
                    name: "constraint_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "table_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "constraint_name",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "constraint_type",
                    oid: types::OID_TEXT,
                },
            ],
            &[&["7", "10", "pk_accounts", "PRIMARY KEY"]],
            "SELECT 1",
        )
        .await;
    }
    if normalized.contains("from sys.procedures") && normalized.contains("union all") {
        return write_result_set(
            stream,
            sequence,
            attachment_id,
            txn_id,
            &[
                RuntimeColumn {
                    name: "routine_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "schema_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "routine_name",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "routine_type",
                    oid: types::OID_TEXT,
                },
            ],
            &[&["12", "1", "sync_profiles", "FUNCTION"]],
            "SELECT 1",
        )
        .await;
    }
    if normalized.contains("from sys.procedures") {
        return write_result_set(
            stream,
            sequence,
            attachment_id,
            txn_id,
            &[
                RuntimeColumn {
                    name: "procedure_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "schema_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "procedure_name",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "routine_type",
                    oid: types::OID_TEXT,
                },
            ],
            &[&["11", "1", "sync_accounts", "PROCEDURE"]],
            "SELECT 1",
        )
        .await;
    }
    if normalized.contains("from sys.functions") {
        return write_result_set(
            stream,
            sequence,
            attachment_id,
            txn_id,
            &[
                RuntimeColumn {
                    name: "function_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "schema_id",
                    oid: types::OID_TEXT,
                },
                RuntimeColumn {
                    name: "function_name",
                    oid: types::OID_TEXT,
                },
            ],
            &[&["12", "1", "sync_profiles"]],
            "SELECT 1",
        )
        .await;
    }

    write_result_set(
        stream,
        sequence,
        attachment_id,
        txn_id,
        &[RuntimeColumn {
            name: "value",
            oid: types::OID_INT4,
        }],
        &[&["1"]],
        "SELECT 1",
    )
    .await
}

async fn write_result_set(
    stream: &mut TcpStream,
    sequence: &mut u32,
    attachment_id: [u8; 16],
    txn_id: u64,
    columns: &[RuntimeColumn],
    rows: &[&[&str]],
    tag: &str,
) -> std::result::Result<(), String> {
    write_protocol_message(
        stream,
        sequence,
        protocol::MSG_ROW_DESCRIPTION,
        &runtime_row_description_payload(columns),
        attachment_id,
        txn_id,
    )
    .await?;
    for row in rows {
        write_protocol_message(
            stream,
            sequence,
            protocol::MSG_DATA_ROW,
            &runtime_data_row_payload(row),
            attachment_id,
            txn_id,
        )
        .await?;
    }
    write_protocol_message(
        stream,
        sequence,
        protocol::MSG_COMMAND_COMPLETE,
        &command_complete_payload(rows.len() as u64, 0, tag),
        attachment_id,
        txn_id,
    )
    .await
}

async fn read_protocol_message(
    stream: &mut TcpStream,
) -> std::result::Result<protocol::Message, String> {
    let mut header_bytes = [0u8; protocol::HEADER_SIZE];
    stream
        .read_exact(&mut header_bytes)
        .await
        .map_err(|e| e.to_string())?;
    let header = protocol::decode_header(&header_bytes).map_err(|e| e.to_string())?;
    let mut payload = vec![0u8; header.length as usize];
    if header.length > 0 {
        stream
            .read_exact(&mut payload)
            .await
            .map_err(|e| e.to_string())?;
    }
    Ok(protocol::Message { header, payload })
}

async fn write_protocol_message(
    stream: &mut TcpStream,
    sequence: &mut u32,
    msg_type: u8,
    payload: &[u8],
    attachment_id: [u8; 16],
    txn_id: u64,
) -> std::result::Result<(), String> {
    let header = MessageHeader {
        msg_type,
        flags: 0,
        length: payload.len() as u32,
        sequence: *sequence,
        attachment_id,
        txn_id,
    };
    *sequence = sequence.wrapping_add(1);
    let bytes = protocol::encode_message(&header, payload);
    stream.write_all(&bytes).await.map_err(|e| e.to_string())
}

fn parse_query_payload(payload: &[u8]) -> std::result::Result<(u32, String), String> {
    if payload.len() < 12 {
        return Err(format!("query payload too short: {}", payload.len()));
    }
    let flags = u32::from_le_bytes(
        payload[0..4]
            .try_into()
            .map_err(|_| "invalid query flags".to_string())?,
    );
    let sql_bytes = &payload[12..];
    let sql_end = sql_bytes
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(sql_bytes.len());
    Ok((
        flags,
        String::from_utf8_lossy(&sql_bytes[..sql_end]).to_string(),
    ))
}

fn parse_set_option_payload(payload: &[u8]) -> std::result::Result<(String, String), String> {
    if payload.len() < 8 {
        return Err(format!("set_option payload too short: {}", payload.len()));
    }
    let name_len = u32::from_le_bytes(
        payload[0..4]
            .try_into()
            .map_err(|_| "invalid set_option name length".to_string())?,
    ) as usize;
    if payload.len() < 4 + name_len + 4 {
        return Err("set_option payload truncated".to_string());
    }
    let name_start = 4;
    let name_end = name_start + name_len;
    let value_len = u32::from_le_bytes(
        payload[name_end..(name_end + 4)]
            .try_into()
            .map_err(|_| "invalid set_option value length".to_string())?,
    ) as usize;
    let value_start = name_end + 4;
    let value_end = value_start + value_len;
    if payload.len() < value_end {
        return Err("set_option payload truncated".to_string());
    }
    let name = String::from_utf8_lossy(&payload[name_start..name_end]).to_string();
    let value = String::from_utf8_lossy(&payload[value_start..value_end]).to_string();
    Ok((name, value))
}

fn runtime_row_description_payload(columns: &[RuntimeColumn]) -> Vec<u8> {
    let mut payload = Vec::with_capacity(64);
    payload.extend_from_slice(&(columns.len() as u16).to_le_bytes());
    payload.extend_from_slice(&0u16.to_le_bytes());
    for (index, column) in columns.iter().enumerate() {
        let name_bytes = column.name.as_bytes();
        payload.extend_from_slice(&(name_bytes.len() as u32).to_le_bytes());
        payload.extend_from_slice(name_bytes);
        payload.extend_from_slice(&0u32.to_le_bytes());
        payload.extend_from_slice(&((index + 1) as u16).to_le_bytes());
        payload.extend_from_slice(&column.oid.to_le_bytes());
        payload.extend_from_slice(&0i16.to_le_bytes());
        payload.extend_from_slice(&0i32.to_le_bytes());
        payload.push(types::FORMAT_TEXT as u8);
        payload.push(1);
        payload.extend_from_slice(&0u16.to_le_bytes());
    }
    payload
}

fn runtime_data_row_payload(values: &[&str]) -> Vec<u8> {
    let count = values.len();
    let null_bytes = ((count + 7) / 8).max(1);
    let mut payload = Vec::new();
    payload.extend_from_slice(&(count as u16).to_le_bytes());
    payload.extend_from_slice(&(null_bytes as u16).to_le_bytes());
    payload.extend_from_slice(&vec![0u8; null_bytes]);
    for value in values {
        let bytes = value.as_bytes();
        payload.extend_from_slice(&(bytes.len() as i32).to_le_bytes());
        payload.extend_from_slice(bytes);
    }
    payload
}

fn command_complete_payload(rows: u64, last_id: u64, tag: &str) -> Vec<u8> {
    let mut payload = Vec::with_capacity(20 + tag.len() + 1);
    payload.push(0);
    payload.extend_from_slice(&[0, 0, 0]);
    payload.extend_from_slice(&rows.to_le_bytes());
    payload.extend_from_slice(&last_id.to_le_bytes());
    payload.extend_from_slice(tag.as_bytes());
    payload.push(0);
    payload
}

fn ready_payload(status: u8, txn_id: u64) -> Vec<u8> {
    let mut payload = vec![0u8; 20];
    payload[0] = status;
    payload[4..12].copy_from_slice(&txn_id.to_le_bytes());
    payload
}

fn auth_request_payload(method: u8) -> Vec<u8> {
    vec![method, 0, 0, 0]
}

fn auth_continue_payload(method: u8, stage: u8, data: &[u8]) -> Vec<u8> {
    let mut payload = Vec::with_capacity(8 + data.len());
    payload.push(method);
    payload.push(stage);
    payload.extend_from_slice(&[0, 0]);
    payload.extend_from_slice(&(data.len() as u32).to_le_bytes());
    payload.extend_from_slice(data);
    payload
}

fn auth_ok_payload(info: &[u8]) -> Vec<u8> {
    let mut payload = Vec::with_capacity(20 + info.len());
    payload.extend_from_slice(&[0x11; 16]);
    payload.extend_from_slice(&(info.len() as u32).to_le_bytes());
    payload.extend_from_slice(info);
    payload
}

fn parse_scram_client_nonce(client_first: &str) -> std::result::Result<String, String> {
    for part in client_first.split(',') {
        if let Some(nonce) = part.strip_prefix("r=") {
            if !nonce.is_empty() {
                return Ok(nonce.to_string());
            }
        }
    }
    Err(format!("SCRAM client nonce missing in '{}'", client_first))
}

const MANAGER_PROTOCOL_MAGIC: u32 = 0x4244_4253;
const MANAGER_PROTOCOL_VERSION: u16 = 0x0101;
const MANAGER_HEADER_SIZE: usize = 12;

const MCP_MSG_CONNECT_RESPONSE: u8 = 0x02;
const MCP_MSG_AUTH_CHALLENGE: u8 = 0x12;
const MCP_MSG_AUTH_RESPONSE: u8 = 0x11;
const MCP_MSG_STATUS_RESPONSE: u8 = 0x64;
const MCP_MSG_HELLO: u8 = 0x65;
const MCP_MSG_AUTH_START: u8 = 0x66;
const MCP_MSG_AUTH_CONTINUE: u8 = 0x67;
const MCP_MSG_DB_CONNECT: u8 = 0x69;

async fn read_manager_frame(stream: &mut TcpStream) -> std::result::Result<(u8, Vec<u8>), String> {
    let mut header = [0u8; MANAGER_HEADER_SIZE];
    stream
        .read_exact(&mut header)
        .await
        .map_err(|e| e.to_string())?;
    let magic = u32::from_le_bytes([header[0], header[1], header[2], header[3]]);
    if magic != MANAGER_PROTOCOL_MAGIC {
        return Err("invalid manager protocol magic".to_string());
    }
    let version = u16::from_le_bytes([header[4], header[5]]);
    if version != MANAGER_PROTOCOL_VERSION {
        return Err("invalid manager protocol version".to_string());
    }
    let msg_type = header[6];
    let payload_len = u32::from_le_bytes([header[8], header[9], header[10], header[11]]) as usize;
    let mut payload = vec![0u8; payload_len];
    if payload_len > 0 {
        stream
            .read_exact(&mut payload)
            .await
            .map_err(|e| e.to_string())?;
    }
    Ok((msg_type, payload))
}

async fn write_manager_frame(
    stream: &mut TcpStream,
    msg_type: u8,
    payload: &[u8],
) -> std::result::Result<(), String> {
    let mut header = [0u8; MANAGER_HEADER_SIZE];
    header[0..4].copy_from_slice(&MANAGER_PROTOCOL_MAGIC.to_le_bytes());
    header[4..6].copy_from_slice(&MANAGER_PROTOCOL_VERSION.to_le_bytes());
    header[6] = msg_type;
    header[7] = 0;
    header[8..12].copy_from_slice(&(payload.len() as u32).to_le_bytes());
    stream.write_all(&header).await.map_err(|e| e.to_string())?;
    stream.write_all(payload).await.map_err(|e| e.to_string())
}

fn extract_manager_auth_start_token_len(payload: &[u8]) -> u32 {
    if payload.len() < 4 {
        return 0;
    }
    let user_len = u32::from_le_bytes(payload[0..4].try_into().unwrap_or([0u8; 4])) as usize;
    let offset = 4 + user_len + 1;
    if payload.len() < offset + 4 {
        return 0;
    }
    u32::from_le_bytes(payload[offset..offset + 4].try_into().unwrap_or([0u8; 4]))
}

fn manager_auth_response_success() -> Vec<u8> {
    let mut payload = vec![0u8; 1 + 4 + 256];
    payload[0] = 0;
    payload
}

fn manager_auth_response_failure(message: &str) -> Vec<u8> {
    let mut payload = vec![0u8; 1 + 4 + 256];
    payload[0] = 1;
    let bytes = message.as_bytes();
    let len = bytes.len().min(255);
    payload[5..(5 + len)].copy_from_slice(&bytes[..len]);
    payload
}

fn manager_connect_response_success() -> Vec<u8> {
    vec![0u8; 1 + 2 + 2 + 16 + 64 + 32]
}

fn build_client_for_server(server: &RuntimeGateServer) -> Client {
    let mut cfg = Config::default();
    cfg.host = server.addr().ip().to_string();
    cfg.port = server.addr().port();
    cfg.user = "alice".to_string();
    cfg.password = "secret".to_string();
    cfg.database = "runtime_db".to_string();
    cfg.sslmode = "disable".to_string();
    Client::new(cfg)
}

#[tokio::test]
async fn runtime_gate_manager_proxy_and_capability_parity() {
    let server = RuntimeGateServer::start(RuntimeGateOptions {
        manager_proxy: true,
        auth_method: protocol::AUTH_TOKEN,
        manager_auth_success: true,
        zero_txn_ready_status: 0,
    })
    .await;

    let mut cfg = Config::default();
    cfg.host = server.addr().ip().to_string();
    cfg.port = server.addr().port();
    cfg.user = "alice".to_string();
    cfg.password = "secret".to_string();
    cfg.database = "runtime_db".to_string();
    cfg.protocol = "jdbc".to_string();
    cfg.sslmode = "disable".to_string();
    cfg.binary_transfer = false;
    cfg.compression = "zstd".to_string();
    cfg.front_door_mode = "manager_proxy".to_string();
    cfg.manager_auth_token = "token".to_string();
    cfg.manager_auth_fast_path = false;
    cfg.auth_payload_json = "{\"token\":\"abc\"}".to_string();

    let mut client = Client::new(cfg);
    client.connect().await.expect("connect");
    let sets = client
        .query_multi("SELECT 1; SELECT 2", Params::from(()))
        .await
        .expect("query multi");
    assert_eq!(sets.len(), 2);
    match &sets[0].rows[0][0] {
        Value::Int32(v) => assert_eq!(*v, 1),
        Value::Int64(v) => assert_eq!(*v, 1),
        Value::String(v) => assert_eq!(v, "1"),
        other => panic!("unexpected first result type: {other:?}"),
    }
    match &sets[1].rows[0][0] {
        Value::Int32(v) => assert_eq!(*v, 2),
        Value::Int64(v) => assert_eq!(*v, 2),
        Value::String(v) => assert_eq!(v, "2"),
        other => panic!("unexpected second result type: {other:?}"),
    }
    client.terminate().await.expect("terminate");
    let snapshot = server.snapshot();
    server.finish().await;

    assert!(snapshot.startup_features & protocol::FEATURE_COMPRESSION != 0);
    assert!(snapshot.startup_features & protocol::FEATURE_STREAMING == 0);
    for flags in snapshot.query_flags {
        assert_eq!(flags & protocol::QUERY_FLAG_BINARY_RESULT, 0);
    }
    assert_eq!(
        snapshot.manager_frames,
        vec![
            MCP_MSG_HELLO,
            MCP_MSG_AUTH_START,
            MCP_MSG_AUTH_CONTINUE,
            MCP_MSG_DB_CONNECT
        ]
    );
    assert_eq!(snapshot.auth_responses.len(), 1);
    assert_eq!(snapshot.auth_responses[0], b"{\"token\":\"abc\"}");
}

#[tokio::test]
async fn runtime_gate_callable_and_batch_helpers_are_deterministic() {
    let server = RuntimeGateServer::start(RuntimeGateOptions::direct(protocol::AUTH_OK)).await;
    let mut client = build_client_for_server(&server);
    client.connect().await.expect("connect");

    let result = client
        .call("{ ? = call abs(-3) }", Params::from(()))
        .await
        .expect("call");
    assert!(!result.rows.is_empty());
    match &result.rows[0][0] {
        Value::Int32(v) => assert_eq!(*v, 3),
        Value::Int64(v) => assert_eq!(*v, 3),
        Value::String(v) => assert_eq!(v, "3"),
        other => panic!("unexpected callable result type: {other:?}"),
    }

    let batch = client
        .execute_batch("SELECT 1", vec![Params::from(()), Params::from(())])
        .await
        .expect("execute batch");
    client.terminate().await.expect("terminate");
    server.finish().await;

    assert_eq!(batch.items.len(), 2);
    assert_eq!(batch.items[0].row_count, 1);
    assert_eq!(batch.items[1].row_count, 1);
    assert_eq!(batch.total_row_count, 2);
}

#[tokio::test]
async fn runtime_gate_generated_key_helper_surfaces_last_insert_id() {
    let server = RuntimeGateServer::start(RuntimeGateOptions::direct(protocol::AUTH_OK)).await;
    let mut client = build_client_for_server(&server);
    client.connect().await.expect("connect");

    let keys = client
        .execute_with_generated_keys(
            "INSERT INTO generated_key_fixture (note) VALUES ('runtime-gate')",
            Params::from(()),
        )
        .await
        .expect("generated keys");
    client.terminate().await.expect("terminate");
    server.finish().await;

    assert_eq!(keys, vec![41]);
}

#[tokio::test]
async fn runtime_gate_manager_proxy_auth_failure_is_deterministic() {
    let server = RuntimeGateServer::start(RuntimeGateOptions {
        manager_proxy: true,
        auth_method: protocol::AUTH_OK,
        manager_auth_success: false,
        zero_txn_ready_status: 0,
    })
    .await;

    let mut cfg = Config::default();
    cfg.host = server.addr().ip().to_string();
    cfg.port = server.addr().port();
    cfg.user = "alice".to_string();
    cfg.password = "secret".to_string();
    cfg.database = "runtime_db".to_string();
    cfg.protocol = "native".to_string();
    cfg.sslmode = "disable".to_string();
    cfg.front_door_mode = "manager_proxy".to_string();
    cfg.manager_auth_token = "token".to_string();

    let mut client = Client::new(cfg);
    let err = client.connect().await.expect_err("connect should fail");
    assert_eq!(err.kind, ErrorKind::Auth);
    assert_eq!(err.sqlstate.as_deref(), Some("28000"));
    let snapshot = server.snapshot();
    server.finish().await;
    assert_eq!(
        snapshot.manager_frames,
        vec![MCP_MSG_HELLO, MCP_MSG_AUTH_START]
    );
}

#[tokio::test]
async fn runtime_gate_password_and_scram_auth_paths() {
    let password_server =
        RuntimeGateServer::start(RuntimeGateOptions::direct(protocol::AUTH_PASSWORD)).await;
    let mut client = build_client_for_server(&password_server);
    client.connect().await.expect("password connect");
    client.terminate().await.expect("password terminate");
    let password_snapshot = password_server.snapshot();
    password_server.finish().await;
    assert_eq!(password_snapshot.auth_responses.len(), 1);
    assert_eq!(password_snapshot.auth_responses[0], b"secret");

    let scram_server =
        RuntimeGateServer::start(RuntimeGateOptions::direct(protocol::AUTH_SCRAM_SHA256)).await;
    let mut client = build_client_for_server(&scram_server);
    client.connect().await.expect("scram connect");
    client.terminate().await.expect("scram terminate");
    let scram_snapshot = scram_server.snapshot();
    scram_server.finish().await;
    assert_eq!(scram_snapshot.auth_responses.len(), 2);
    let first = String::from_utf8_lossy(&scram_snapshot.auth_responses[0]);
    let second = String::from_utf8_lossy(&scram_snapshot.auth_responses[1]);
    assert!(first.starts_with("n,,n=alice,r="));
    assert!(second.contains("c=biws"));

    let scram512_server =
        RuntimeGateServer::start(RuntimeGateOptions::direct(protocol::AUTH_SCRAM_SHA512)).await;
    let mut client = build_client_for_server(&scram512_server);
    client.connect().await.expect("scram512 connect");
    let scram512_context = client.get_resolved_auth_context();
    client.terminate().await.expect("scram512 terminate");
    let scram512_snapshot = scram512_server.snapshot();
    scram512_server.finish().await;
    assert_eq!(scram512_snapshot.auth_responses.len(), 2);
    assert_eq!(scram512_context.resolved_auth_method, "SCRAM_SHA_512");

    let token_server =
        RuntimeGateServer::start(RuntimeGateOptions::direct(protocol::AUTH_TOKEN)).await;
    let mut cfg = Config::default();
    cfg.host = token_server.addr().ip().to_string();
    cfg.port = token_server.addr().port();
    cfg.user = "alice".to_string();
    cfg.password = "secret".to_string();
    cfg.database = "test".to_string();
    cfg.sslmode = "disable".to_string();
    cfg.auth_token = "bearer-token".to_string();
    let mut client = Client::new(cfg);
    client.connect().await.expect("token connect");
    let token_context = client.get_resolved_auth_context();
    client.terminate().await.expect("token terminate");
    let token_snapshot = token_server.snapshot();
    token_server.finish().await;
    assert_eq!(token_snapshot.auth_responses.len(), 1);
    assert_eq!(token_snapshot.auth_responses[0], b"bearer-token");
    assert_eq!(token_context.resolved_auth_method, "TOKEN");
}

#[tokio::test]
async fn runtime_gate_probe_auth_surface_reports_direct_and_manager_modes() {
    let direct_server =
        RuntimeGateServer::start(RuntimeGateOptions::direct(protocol::AUTH_SCRAM_SHA512)).await;
    let mut client = build_client_for_server(&direct_server);
    let direct_probe = client.probe_auth_surface().await.expect("direct probe");
    let direct_snapshot = direct_server.snapshot();
    direct_server.finish().await;
    assert!(direct_probe.reachable);
    assert_eq!(direct_probe.front_door_mode, "direct");
    assert_eq!(direct_probe.required_method, "SCRAM_SHA_512");
    assert_eq!(direct_probe.admitted_methods.len(), 1);
    assert_eq!(
        direct_probe.admitted_methods[0].method_name,
        "SCRAM_SHA_512"
    );
    assert!(direct_snapshot.auth_responses.is_empty());

    let manager_server = RuntimeGateServer::start(RuntimeGateOptions {
        manager_proxy: true,
        auth_method: protocol::AUTH_OK,
        manager_auth_success: true,
        zero_txn_ready_status: 0,
    })
    .await;
    let mut cfg = Config::default();
    cfg.host = manager_server.addr().ip().to_string();
    cfg.port = manager_server.addr().port();
    cfg.user = "alice".to_string();
    cfg.database = "runtime_db".to_string();
    cfg.sslmode = "disable".to_string();
    cfg.front_door_mode = "manager_proxy".to_string();
    let mut client = Client::new(cfg);
    let manager_probe = client.probe_auth_surface().await.expect("manager probe");
    let manager_snapshot = manager_server.snapshot();
    manager_server.finish().await;
    assert!(manager_probe.reachable);
    assert_eq!(manager_probe.front_door_mode, "manager_proxy");
    assert_eq!(manager_probe.required_method, "TOKEN");
    assert_eq!(manager_probe.admitted_methods.len(), 1);
    assert_eq!(manager_probe.admitted_methods[0].method_name, "TOKEN");
    assert_eq!(
        manager_snapshot.manager_frames,
        vec![MCP_MSG_HELLO, MCP_MSG_AUTH_START]
    );
}

#[tokio::test]
async fn runtime_gate_fail_closed_peer_preserves_resolved_auth_context() {
    let server = RuntimeGateServer::start(RuntimeGateOptions::direct(protocol::AUTH_PEER)).await;
    let mut client = build_client_for_server(&server);
    let err = client.connect().await.expect_err("peer should fail closed");
    assert_eq!(err.kind, ErrorKind::NotSupported);
    assert_eq!(err.sqlstate.as_deref(), Some("0A000"));
    let context = client.get_resolved_auth_context();
    let snapshot = server.snapshot();
    server.finish().await;
    assert_eq!(context.resolved_auth_method, "PEER");
    assert_eq!(context.resolved_auth_plugin_id, "scratchbird.auth.peer_uid");
    assert!(!context.attached);
    assert_eq!(snapshot.auth_responses.len(), 0);
}

#[tokio::test]
async fn runtime_gate_autocommit_transition_semantics() {
    let server = RuntimeGateServer::start(RuntimeGateOptions {
        zero_txn_ready_status: b'T',
        ..RuntimeGateOptions::direct(protocol::AUTH_OK)
    })
    .await;
    let mut client = build_client_for_server(&server);
    client.connect().await.expect("connect");
    assert!(client.autocommit());
    client
        .set_autocommit(false)
        .await
        .expect("set autocommit false");
    assert!(!client.autocommit());
    client
        .set_autocommit(true)
        .await
        .expect("set autocommit true");
    assert!(client.autocommit());
    client
        .set_autocommit(true)
        .await
        .expect("set autocommit true no-op");
    client.terminate().await.expect("terminate");
    let snapshot = server.snapshot();
    server.finish().await;

    assert_eq!(snapshot.events, vec!["txn_commit".to_string()]);
    assert!(snapshot.set_options.is_empty());
}

#[tokio::test]
async fn runtime_gate_metadata_matrix_and_ddl_payload() {
    let server = RuntimeGateServer::start(RuntimeGateOptions::direct(protocol::AUTH_OK)).await;
    let mut client = build_client_for_server(&server);
    client.connect().await.expect("connect");

    let cases: Vec<(&str, HashMap<String, String>)> = vec![
        (
            "catalogs",
            HashMap::from([("catalog".to_string(), "users.alice".to_string())]),
        ),
        (
            "schemas",
            HashMap::from([("schema".to_string(), "users.alice".to_string())]),
        ),
        (
            "tables",
            HashMap::from([
                ("schema".to_string(), "users.alice".to_string()),
                ("table".to_string(), "accounts".to_string()),
            ]),
        ),
        (
            "columns",
            HashMap::from([
                ("table".to_string(), "accounts".to_string()),
                ("column".to_string(), "account_id".to_string()),
            ]),
        ),
        (
            "indexes",
            HashMap::from([("index".to_string(), "idx_accounts_id".to_string())]),
        ),
        (
            "index_columns",
            HashMap::from([
                ("index".to_string(), "idx_accounts_id".to_string()),
                ("column".to_string(), "account_id".to_string()),
            ]),
        ),
        (
            "constraints",
            HashMap::from([("constraint".to_string(), "pk_accounts".to_string())]),
        ),
        (
            "primary_keys",
            HashMap::from([("constraint".to_string(), "pk_accounts".to_string())]),
        ),
        (
            "foreign_keys",
            HashMap::from([("constraint".to_string(), "fk_accounts_owner".to_string())]),
        ),
        (
            "table_privileges",
            HashMap::from([("table".to_string(), "accounts".to_string())]),
        ),
        (
            "column_privileges",
            HashMap::from([("column".to_string(), "account_id".to_string())]),
        ),
        (
            "procedures",
            HashMap::from([("procedure".to_string(), "sync_accounts".to_string())]),
        ),
        (
            "functions",
            HashMap::from([("function".to_string(), "sync_profiles".to_string())]),
        ),
        (
            "routines",
            HashMap::from([("routine".to_string(), "sync_profiles".to_string())]),
        ),
        (
            "type_info",
            HashMap::from([("type".to_string(), "INT4".to_string())]),
        ),
    ];

    for (collection, restrictions) in cases {
        let result = client
            .query_metadata_with_restrictions(collection, &restrictions)
            .await
            .expect("metadata query");
        assert!(!result.rows.is_empty(), "expected rows for {collection}");
        assert!(result.row_count > 0, "expected row_count for {collection}");
    }

    let schemas = client
        .get_schema("schemas", None)
        .await
        .expect("get schema");
    assert!(!schemas.is_empty());
    let tree = client
        .get_schema_tree(Some("users.%"), Some(true))
        .await
        .expect("schema tree");
    assert!(!tree.schemas.is_empty());
    let payload = client
        .ddl_editor_schema_payload(Some("users.%"), Some(true))
        .await
        .expect("ddl payload");
    assert_eq!(payload["expandSchemaParents"].as_bool(), Some(true));
    assert!(payload["schemaPaths"]
        .as_array()
        .map(|values| !values.is_empty())
        .unwrap_or(false));
    assert!(payload["schemaTree"]
        .as_array()
        .map(|values| !values.is_empty())
        .unwrap_or(false));

    client.terminate().await.expect("terminate");
    server.finish().await;
}
