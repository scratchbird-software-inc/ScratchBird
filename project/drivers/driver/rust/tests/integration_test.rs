// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

use std::collections::HashMap;
use std::env;
use std::time::Duration;

use scratchbird::sql::Params;
use scratchbird::types::{Param, Value};
use scratchbird::{Client, Config, ErrorKind};
use tokio::time::timeout;

fn is_not_supported(err: &scratchbird::Error) -> bool {
    err.kind == ErrorKind::NotSupported || err.sqlstate.as_deref() == Some("0A000")
}

#[tokio::test]
async fn basic_query() {
    let dsn = match env::var("SCRATCHBIRD_RUST_URL") {
        Ok(val) => val,
        Err(_) => return,
    };
    let config = Config::from_dsn(&dsn).unwrap();
    let mut client = Client::new(config);
    client.connect().await.unwrap();
    let result = client.query("SELECT 1").await.unwrap();
    client.close().await;
    assert!(!result.rows.is_empty());
}

#[tokio::test]
async fn prepare_bind_query() {
    let dsn = match env::var("SCRATCHBIRD_RUST_URL") {
        Ok(val) => val,
        Err(_) => return,
    };
    let config = Config::from_dsn(&dsn).unwrap();
    let mut client = Client::new(config);
    client.connect().await.unwrap();
    let result = timeout(
        Duration::from_secs(5),
        client.query_params("SELECT ?::INTEGER", Params::from(vec![Param::Int32(42)])),
    )
    .await
    .expect("prepare_bind_query timed out")
    .expect("prepare_bind_query returned query error");
    client.close().await;
    assert!(!result.rows.is_empty());
    match result.rows[0][0] {
        Value::Int32(v) => assert_eq!(v, 42),
        Value::Int64(v) => assert_eq!(v, 42),
        _ => panic!("unexpected value type"),
    }
}

#[tokio::test]
async fn types_fixture_query() {
    let dsn = match env::var("SCRATCHBIRD_RUST_URL") {
        Ok(val) => val,
        Err(_) => return,
    };
    let config = Config::from_dsn(&dsn).unwrap();
    let mut client = Client::new(config);
    client.connect().await.unwrap();
    let result = client
        .query("SELECT COUNT(*) FROM type_coverage")
        .await
        .unwrap();
    client.close().await;
    assert!(!result.rows.is_empty());
}

#[tokio::test]
async fn transaction_stays_active_across_connect_commit_and_rollback() {
    let dsn = match env::var("SCRATCHBIRD_RUST_URL") {
        Ok(val) => val,
        Err(_) => return,
    };
    let config = Config::from_dsn(&dsn).unwrap();
    let mut client = Client::new(config);
    client.connect().await.unwrap();
    client.savepoint("sp_rust_bootstrap").await.unwrap();
    client.release_savepoint("sp_rust_bootstrap").await.unwrap();
    client.begin(None).await.unwrap();
    client.commit(None).await.unwrap();
    client.savepoint("sp_rust_after_commit").await.unwrap();
    client.release_savepoint("sp_rust_after_commit").await.unwrap();
    client.begin(None).await.unwrap();
    client.rollback(None).await.unwrap();
    client.savepoint("sp_rust_after_rollback").await.unwrap();
    client.release_savepoint("sp_rust_after_rollback").await.unwrap();
    client.close().await;
}

#[tokio::test]
async fn nested_begin_is_rejected_while_active() {
    let dsn = match env::var("SCRATCHBIRD_RUST_URL") {
        Ok(val) => val,
        Err(_) => return,
    };
    let config = Config::from_dsn(&dsn).unwrap();
    let mut client = Client::new(config);
    client.connect().await.unwrap();
    client.begin(None).await.unwrap();
    let err = client.begin(None).await.unwrap_err();
    client.close().await;
    assert_eq!(err.kind, ErrorKind::Transaction);
    assert_eq!(err.sqlstate.as_deref(), Some("25000"));
}

#[tokio::test]
async fn post_rollback_query_observes_actual_result_after_fresh_reopen() {
    let dsn = match env::var("SCRATCHBIRD_RUST_URL") {
        Ok(val) => val,
        Err(_) => return,
    };
    let config = Config::from_dsn(&dsn).unwrap();
    let mut client = Client::new(config);
    client.connect().await.unwrap();
    client.begin(None).await.unwrap();
    client.rollback(None).await.unwrap();
    let result = client.query("SELECT 2").await.unwrap();
    client.close().await;
    assert!(!result.rows.is_empty());
    match result.rows[0][0] {
        Value::Int32(v) => assert_eq!(v, 2),
        Value::Int64(v) => assert_eq!(v, 2),
        _ => panic!("unexpected value type"),
    }
}

#[tokio::test]
async fn query_multi_returns_independent_result_sets() {
    let dsn = match env::var("SCRATCHBIRD_RUST_URL") {
        Ok(val) => val,
        Err(_) => return,
    };
    let config = Config::from_dsn(&dsn).unwrap();
    let mut client = Client::new(config);
    client.connect().await.unwrap();
    let result = client
        .query_multi(
            "SELECT 1 AS first_value; SELECT 2 AS second_value",
            Params::from(()),
        )
        .await;
    let result = match result {
        Ok(value) => value,
        Err(err) if is_not_supported(&err) => return,
        Err(err) => panic!("query_multi failed: {}", err),
    };
    client.close().await;
    assert_eq!(result.len(), 2);
    match result[0].rows[0][0] {
        Value::Int32(v) => assert_eq!(v, 1),
        Value::Int64(v) => assert_eq!(v, 1),
        _ => panic!("unexpected first result value type"),
    }
    match result[1].rows[0][0] {
        Value::Int32(v) => assert_eq!(v, 2),
        Value::Int64(v) => assert_eq!(v, 2),
        _ => panic!("unexpected second result value type"),
    }
}

#[tokio::test]
async fn execute_batch_returns_summary() {
    let dsn = match env::var("SCRATCHBIRD_RUST_URL") {
        Ok(val) => val,
        Err(_) => return,
    };
    let config = Config::from_dsn(&dsn).unwrap();
    let mut client = Client::new(config);
    client.connect().await.unwrap();
    let batch = client
        .execute_batch(
            "SELECT ?::INTEGER AS value",
            vec![
                Params::Positional(vec![Param::from(11_i32)]),
                Params::Positional(vec![Param::from(22_i32)]),
            ],
        )
        .await;
    let batch = match batch {
        Ok(value) => value,
        Err(err) if is_not_supported(&err) => return,
        Err(err) => panic!("execute_batch failed: {}", err),
    };
    client.close().await;
    assert_eq!(batch.items.len(), 2);
    assert_eq!(batch.items[0].index, 0);
    assert_eq!(batch.items[1].index, 1);
}

#[tokio::test]
async fn call_executes_callable_escape_syntax() {
    let dsn = match env::var("SCRATCHBIRD_RUST_URL") {
        Ok(val) => val,
        Err(_) => return,
    };
    let config = Config::from_dsn(&dsn).unwrap();
    let mut client = Client::new(config);
    client.connect().await.unwrap();
    let result = client
        .call(
            "{ ? = call abs(?) }",
            Params::Positional(vec![Param::from(-3_i32)]),
        )
        .await;
    let result = match result {
        Ok(value) => value,
        Err(err) if is_not_supported(&err) => return,
        Err(err) => panic!("call failed: {}", err),
    };
    client.close().await;
    assert!(!result.rows.is_empty());
    match result.rows[0][0] {
        Value::Int32(v) => assert_eq!(v, 3),
        Value::Int64(v) => assert_eq!(v, 3),
        _ => panic!("unexpected callable return value type"),
    }
}

#[tokio::test]
async fn execute_with_generated_keys_returns_vec() {
    let dsn = match env::var("SCRATCHBIRD_RUST_URL") {
        Ok(val) => val,
        Err(_) => return,
    };
    let config = Config::from_dsn(&dsn).unwrap();
    let mut client = Client::new(config);
    client.connect().await.unwrap();
    let result = client
        .execute_with_generated_keys("SELECT 1 AS one", Params::from(()))
        .await;
    let result = match result {
        Ok(value) => value,
        Err(err) if is_not_supported(&err) => return,
        Err(err) => panic!("execute_with_generated_keys failed: {}", err),
    };
    client.close().await;
    assert!(result.is_empty() || result.iter().all(|id| *id >= 0));
}

#[tokio::test]
async fn query_metadata_with_restrictions_filters_schema_rows() {
    let dsn = match env::var("SCRATCHBIRD_RUST_URL") {
        Ok(val) => val,
        Err(_) => return,
    };
    let config = Config::from_dsn(&dsn).unwrap();
    let mut client = Client::new(config);
    client.connect().await.unwrap();

    let all_schemas = client.query_metadata("schemas").await.unwrap();
    if all_schemas.rows.is_empty() {
        client.close().await;
        return;
    }

    let schema_col = all_schemas.columns.iter().position(|col| {
        col.name.eq_ignore_ascii_case("schema_name")
            || col.name.eq_ignore_ascii_case("TABLE_SCHEM")
            || col.name.eq_ignore_ascii_case("table_schema")
    });
    let Some(schema_col) = schema_col else {
        client.close().await;
        return;
    };

    let target_schema = match all_schemas.rows[0].get(schema_col) {
        Some(Value::String(value)) if !value.is_empty() => value.clone(),
        _ => {
            client.close().await;
            return;
        }
    };

    let restrictions = HashMap::from([("schema".to_string(), target_schema.clone())]);
    let filtered = client
        .query_metadata_with_restrictions("schemas", &restrictions)
        .await
        .unwrap();
    client.close().await;

    assert!(!filtered.rows.is_empty());
    for row in filtered.rows {
        match row.get(schema_col) {
            Some(Value::String(value)) => assert_eq!(value, &target_schema),
            _ => panic!("unexpected schema value type"),
        }
    }
}

#[tokio::test]
async fn cancel_query() {
    let dsn = match env::var("SCRATCHBIRD_RUST_URL") {
        Ok(val) => val,
        Err(_) => return,
    };
    let cancel_sql = match env::var("SCRATCHBIRD_RUST_CANCEL_SQL") {
        Ok(val) => val,
        Err(_) => return,
    };
    let config = Config::from_dsn(&dsn).unwrap();
    let mut client = Client::new(config);
    client.connect().await.unwrap();
    let result = timeout(Duration::from_millis(200), client.query(&cancel_sql)).await;
    if result.is_err() {
        let _ = client.cancel().await;
    }
    client.close().await;
}
