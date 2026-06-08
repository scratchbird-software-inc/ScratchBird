// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

use scratchbird::{normalize, normalize_callable, normalize_callable_sql, Param, Params};

#[test]
fn normalize_positional() {
    let sql = "SELECT * FROM t WHERE id = ? AND name = ?";
    let normalized = normalize(
        sql,
        Params::Positional(vec![Param::from(42_i64), Param::from("Ada")]),
    )
    .unwrap();
    assert_eq!(
        normalized.sql,
        "SELECT * FROM t WHERE id = $1 AND name = $2"
    );
    assert_eq!(normalized.params.len(), 2);
}

#[test]
fn normalize_named() {
    let sql = "SELECT * FROM users WHERE name = @name AND active = :active";
    let mut params = std::collections::HashMap::new();
    params.insert("name".to_string(), Param::from("Ada"));
    params.insert("active".to_string(), Param::from(true));
    let normalized = normalize(sql, Params::Named(params)).unwrap();
    assert_eq!(
        normalized.sql,
        "SELECT * FROM users WHERE name = $1 AND active = $2"
    );
    assert_eq!(normalized.params.len(), 2);
}

#[test]
fn normalize_positional_ignores_escaped_string_literals() {
    let sql = "SELECT 'it''s ?' AS txt, ?::INTEGER";
    let normalized = normalize(sql, Params::Positional(vec![Param::from(42_i64)])).unwrap();
    assert_eq!(normalized.sql, "SELECT 'it''s ?' AS txt, $1::INTEGER");
    assert_eq!(normalized.params.len(), 1);
}

#[test]
fn normalize_named_ignores_escaped_string_literals() {
    let sql = "SELECT 'it''s @name' AS txt, @name";
    let mut params = std::collections::HashMap::new();
    params.insert("name".to_string(), Param::from("Ada"));
    let normalized = normalize(sql, Params::Named(params)).unwrap();
    assert_eq!(normalized.sql, "SELECT 'it''s @name' AS txt, $1");
    assert_eq!(normalized.params.len(), 1);
}

#[test]
fn normalize_named_errors_when_placeholders_exist_only_in_literals() {
    let sql = "SELECT 'it''s @name' AS txt";
    let mut params = std::collections::HashMap::new();
    params.insert("name".to_string(), Param::from("Ada"));
    let err = normalize(sql, Params::Named(params)).unwrap_err();
    assert_eq!(
        err.message,
        "named parameters provided but query has no placeholders"
    );
}

#[test]
fn normalize_callable_invocation_rewrites_escape_sequence() {
    let normalized = normalize_callable(
        "{ call calculate(?, ?) }",
        Params::Positional(vec![Param::from(2_i32), Param::from(3_i32)]),
    )
    .unwrap();
    assert_eq!(normalized.sql, "call calculate($1, $2)");
    assert_eq!(normalized.params.len(), 2);
}

#[test]
fn normalize_callable_return_value_select() {
    let normalized = normalize_callable(
        "{ ? = call abs(?) }",
        Params::Positional(vec![Param::from(-42_i32)]),
    )
    .unwrap();
    assert_eq!(normalized.sql, "select abs($1) as return_value");
    assert_eq!(normalized.params.len(), 1);
}

#[test]
fn normalize_callable_sql_passthrough_for_non_escape() {
    let normalized = normalize_callable_sql("SELECT 1").unwrap();
    assert_eq!(normalized, "SELECT 1");
}
