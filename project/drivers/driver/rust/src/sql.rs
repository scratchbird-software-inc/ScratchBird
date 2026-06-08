// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

use std::collections::HashMap;

use crate::errors::{Error, ErrorKind, Result};
use crate::types::Param;

#[derive(Debug, Clone)]
pub enum Params {
    Positional(Vec<Param>),
    Named(HashMap<String, Param>),
}

#[derive(Debug, Clone)]
pub struct NormalizedQuery {
    pub sql: String,
    pub params: Vec<Param>,
}

impl From<Vec<Param>> for Params {
    fn from(value: Vec<Param>) -> Self {
        Params::Positional(value)
    }
}

impl From<HashMap<String, Param>> for Params {
    fn from(value: HashMap<String, Param>) -> Self {
        Params::Named(value)
    }
}

impl From<()> for Params {
    fn from(_: ()) -> Self {
        Params::Positional(Vec::new())
    }
}

pub fn normalize(sql: &str, params: Params) -> Result<NormalizedQuery> {
    match params {
        Params::Positional(values) => {
            if sql.contains('?') {
                let (rewritten, ordered) = rewrite_positional(sql, &values)?;
                Ok(NormalizedQuery {
                    sql: rewritten,
                    params: ordered,
                })
            } else {
                Ok(NormalizedQuery {
                    sql: sql.to_string(),
                    params: values,
                })
            }
        }
        Params::Named(values) => {
            if !has_named_params(sql) {
                return Err(Error::new(
                    ErrorKind::Data,
                    "named parameters provided but query has no placeholders",
                ));
            }
            let (rewritten, ordered) = rewrite_named(sql, &values)?;
            Ok(NormalizedQuery {
                sql: rewritten,
                params: ordered,
            })
        }
    }
}

pub fn normalize_callable(sql: &str, params: Params) -> Result<NormalizedQuery> {
    let callable_sql = normalize_callable_sql(sql)?;
    normalize(&callable_sql, params)
}

pub fn split_top_level_statements(sql: &str) -> Vec<String> {
    let mut statements = Vec::new();
    let mut current = String::new();
    let mut in_single = false;
    let mut in_double = false;

    for ch in sql.chars() {
        if ch == '\'' && !in_double {
            in_single = !in_single;
            current.push(ch);
            continue;
        }
        if ch == '"' && !in_single {
            in_double = !in_double;
            current.push(ch);
            continue;
        }
        if !in_single && !in_double && ch == ';' {
            let statement = current.trim();
            if !statement.is_empty() {
                statements.push(statement.to_string());
            }
            current.clear();
            continue;
        }
        current.push(ch);
    }

    let statement = current.trim();
    if !statement.is_empty() {
        statements.push(statement.to_string());
    }

    statements
}

pub fn normalize_callable_sql(sql: &str) -> Result<String> {
    let trimmed = sql.trim();
    if !(trimmed.starts_with('{') && trimmed.ends_with('}')) {
        return Ok(sql.to_string());
    }
    let inner = trimmed[1..trimmed.len() - 1].trim();
    if inner.is_empty() {
        return Ok(sql.to_string());
    }

    if let Some(after_qmark) = inner.strip_prefix('?') {
        let after_qmark = after_qmark.trim_start();
        if let Some(after_eq) = after_qmark.strip_prefix('=') {
            let after_eq = after_eq.trim_start();
            if starts_with_call(after_eq) {
                let invocation = parse_callable_invocation(after_eq[4..].trim_start())?;
                let call_args = if invocation.has_parens {
                    invocation.args
                } else {
                    String::new()
                };
                return Ok(format!(
                    "select {}({}) as return_value",
                    invocation.routine, call_args
                ));
            }
        }
    }

    if starts_with_call(inner) {
        let invocation = parse_callable_invocation(inner[4..].trim_start())?;
        if invocation.has_parens {
            return Ok(format!("call {}({})", invocation.routine, invocation.args));
        }
        return Ok(format!("call {}", invocation.routine));
    }

    Ok(sql.to_string())
}

fn has_named_params(sql: &str) -> bool {
    let mut in_string = false;
    let chars: Vec<char> = sql.chars().collect();
    let mut i = 0;
    while i + 1 < chars.len() {
        let ch = chars[i];
        if ch == '\'' {
            if in_string && i + 1 < chars.len() && chars[i + 1] == '\'' {
                i += 2;
                continue;
            }
            in_string = !in_string;
            i += 1;
            continue;
        }
        if !in_string && (ch == ':' || ch == '@') && is_ident_start(chars[i + 1]) {
            return true;
        }
        i += 1;
    }
    false
}

fn rewrite_named(sql: &str, params: &HashMap<String, Param>) -> Result<(String, Vec<Param>)> {
    let mut out = String::new();
    let mut ordered = Vec::new();
    let chars: Vec<char> = sql.chars().collect();
    let mut in_string = false;
    let mut i = 0;
    while i < chars.len() {
        let ch = chars[i];
        if ch == '\'' {
            out.push(ch);
            if in_string && i + 1 < chars.len() && chars[i + 1] == '\'' {
                out.push(chars[i + 1]);
                i += 2;
                continue;
            }
            in_string = !in_string;
            i += 1;
            continue;
        }
        if !in_string
            && (ch == ':' || ch == '@')
            && i + 1 < chars.len()
            && is_ident_start(chars[i + 1])
        {
            let mut j = i + 1;
            while j < chars.len() && is_ident_part(chars[j]) {
                j += 1;
            }
            let key: String = chars[i + 1..j].iter().collect();
            let value = params.get(&key).ok_or_else(|| {
                Error::new(ErrorKind::Data, format!("missing named parameter: {}", key))
            })?;
            ordered.push(value.clone());
            out.push('$');
            out.push_str(&(ordered.len()).to_string());
            i = j;
            continue;
        }
        out.push(ch);
        i += 1;
    }
    Ok((out, ordered))
}

fn rewrite_positional(sql: &str, params: &[Param]) -> Result<(String, Vec<Param>)> {
    let mut out = String::new();
    let mut ordered = Vec::new();
    let chars: Vec<char> = sql.chars().collect();
    let mut in_string = false;
    let mut index = 0;
    let mut i = 0;
    while i < chars.len() {
        let ch = chars[i];
        if ch == '\'' {
            out.push(ch);
            if in_string && i + 1 < chars.len() && chars[i + 1] == '\'' {
                out.push(chars[i + 1]);
                i += 2;
                continue;
            }
            in_string = !in_string;
            i += 1;
            continue;
        }
        if !in_string && ch == '?' {
            if index >= params.len() {
                return Err(Error::new(ErrorKind::Data, "not enough parameters"));
            }
            ordered.push(params[index].clone());
            index += 1;
            out.push('$');
            out.push_str(&(ordered.len()).to_string());
            i += 1;
            continue;
        }
        out.push(ch);
        i += 1;
    }
    if index < params.len() {
        return Err(Error::new(ErrorKind::Data, "too many parameters"));
    }
    Ok((out, ordered))
}

fn is_ident_start(ch: char) -> bool {
    ch.is_ascii_alphabetic() || ch == '_'
}

fn is_ident_part(ch: char) -> bool {
    is_ident_start(ch) || ch.is_ascii_digit()
}

fn starts_with_call(value: &str) -> bool {
    value
        .get(0..4)
        .map(|prefix| prefix.eq_ignore_ascii_case("call"))
        .unwrap_or(false)
}

struct CallableInvocation {
    routine: String,
    args: String,
    has_parens: bool,
}

fn parse_callable_invocation(value: &str) -> Result<CallableInvocation> {
    let open_paren = match value.find('(') {
        Some(index) => index,
        None => {
            let routine = value.trim();
            if routine.is_empty() {
                return Err(Error::new(
                    ErrorKind::Data,
                    "invalid JDBC escape call syntax",
                ));
            }
            return Ok(CallableInvocation {
                routine: routine.to_string(),
                args: String::new(),
                has_parens: false,
            });
        }
    };

    let mut in_single = false;
    let mut in_double = false;
    let mut depth = 0;
    let mut close_paren = None;
    for (idx, ch) in value.char_indices().skip(open_paren) {
        if ch == '\'' && !in_double {
            in_single = !in_single;
            continue;
        }
        if ch == '"' && !in_single {
            in_double = !in_double;
            continue;
        }
        if in_single || in_double {
            continue;
        }
        if ch == '(' {
            depth += 1;
            continue;
        }
        if ch == ')' {
            depth -= 1;
            if depth == 0 {
                close_paren = Some(idx);
                break;
            }
        }
    }

    let close_paren = close_paren
        .ok_or_else(|| Error::new(ErrorKind::Data, "invalid JDBC escape call syntax"))?;
    let routine = value[..open_paren].trim();
    if routine.is_empty() {
        return Err(Error::new(
            ErrorKind::Data,
            "invalid JDBC escape call syntax",
        ));
    }
    let trailing = value[close_paren + 1..].trim();
    if !trailing.is_empty() {
        return Err(Error::new(
            ErrorKind::Data,
            "invalid JDBC escape call syntax",
        ));
    }
    let args = value[open_paren + 1..close_paren].trim().to_string();
    Ok(CallableInvocation {
        routine: routine.to_string(),
        args,
        has_parens: true,
    })
}
