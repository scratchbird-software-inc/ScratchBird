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

/// Detect a `SET TERM <terminator>` client directive in a cut chunk.
///
/// Leading full-line `--` comments and blank lines are ignored when matching,
/// so a directive may be preceded by comment lines in the same chunk. Returns
/// the new terminator string if `chunk` is a directive, otherwise `None`.
fn chunk_set_term(chunk: &str) -> Option<String> {
    let mut meaningful: Vec<&str> = Vec::new();
    for line in chunk.lines() {
        let stripped = line.trim();
        if stripped.is_empty() || stripped.starts_with("--") {
            continue;
        }
        meaningful.push(stripped);
    }
    if meaningful.is_empty() {
        return None;
    }
    let joined = meaningful.join(" ");

    // Match `set term <rest>` case-insensitively with a non-empty <rest>.
    let lower = joined.to_ascii_lowercase();
    let after = lower.strip_prefix("set")?;
    if !after.starts_with(|c: char| c.is_whitespace()) {
        return None;
    }
    let after = after.trim_start();
    let after = after.strip_prefix("term")?;
    if !after.starts_with(|c: char| c.is_whitespace()) {
        return None;
    }
    // Locate the same offset in the original (non-lowercased) string to keep the
    // terminator's original case, then trim it.
    let consumed = joined.len() - after.trim_start().len();
    let terminator = joined[consumed..].trim();
    if terminator.is_empty() {
        None
    } else {
        Some(terminator.to_string())
    }
}

/// Split SQL into top-level statements on the active terminator.
///
/// Quote-aware (single/double quotes) and `--` line-comment aware. Honors the
/// `SET TERM <terminator>` client directive (Firebird / `sb_isql` semantics):
/// the directive changes the active terminator and is consumed — it is not
/// emitted as a statement and is not counted in statement indexing. This lets
/// procedural bodies (functions, procedures, triggers) contain inner `;`
/// between `SET TERM ^` and the restoring `SET TERM ;^`.
///
/// With no `SET TERM` directive present, the behavior is identical to a plain
/// quote-aware top-level `;` split, so existing scripts and statement indices
/// are unchanged. (The chosen terminator must not appear in the bodies it
/// wraps.)
pub fn split_top_level_statements(sql: &str) -> Vec<String> {
    let mut statements: Vec<String> = Vec::new();
    let mut term: String = ";".to_string();
    let mut buf = String::new();
    let mut in_single = false;
    let mut in_double = false;
    let chars: Vec<char> = sql.chars().collect();
    let len = chars.len();
    let mut i = 0;

    fn flush(buf: &mut String, term: &mut String, statements: &mut Vec<String>) {
        let chunk = buf.trim();
        if chunk.is_empty() {
            buf.clear();
            return;
        }
        if let Some(new_term) = chunk_set_term(chunk) {
            *term = new_term;
        } else {
            statements.push(chunk.to_string());
        }
        buf.clear();
    }

    while i < len {
        let ch = chars[i];
        if !in_single && !in_double && ch == '-' && i + 1 < len && chars[i + 1] == '-' {
            // `--` line comment: copy verbatim to end of line (or input) without
            // scanning for the terminator or quotes inside it.
            let mut j = i;
            while j < len && chars[j] != '\n' {
                j += 1;
            }
            buf.extend(&chars[i..j]);
            i = j;
            continue;
        }
        if ch == '\'' && !in_double {
            in_single = !in_single;
            buf.push(ch);
            i += 1;
            continue;
        }
        if ch == '"' && !in_single {
            in_double = !in_double;
            buf.push(ch);
            i += 1;
            continue;
        }
        if !in_single && !in_double && !term.is_empty() && starts_with_at(&chars, i, &term) {
            // Capture the matched terminator length BEFORE flushing, because
            // processing the chunk may change the active terminator.
            let matched_len = term.chars().count();
            flush(&mut buf, &mut term, &mut statements);
            i += matched_len;
            continue;
        }
        buf.push(ch);
        i += 1;
    }
    flush(&mut buf, &mut term, &mut statements);
    statements
}

/// True if the `term` string matches the `chars` slice starting at index `i`.
fn starts_with_at(chars: &[char], i: usize, term: &str) -> bool {
    let mut idx = i;
    for tc in term.chars() {
        if idx >= chars.len() || chars[idx] != tc {
            return false;
        }
        idx += 1;
    }
    true
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

#[cfg(test)]
mod chunker_conformance_tests {
    use super::split_top_level_statements;

    /// Load the shared cross-driver chunker fixture and assert that the ported
    /// `split_top_level_statements` reproduces every `expected` list exactly.
    ///
    /// Mirrors `tests/conformance/drivers/chunker_conformance/verify_python_reference.py`.
    #[test]
    fn matches_cross_driver_fixture() {
        // CARGO_MANIFEST_DIR points at .../project/drivers/driver/rust.
        let cases_path = format!(
            "{}/../../../tests/conformance/drivers/chunker_conformance/cases.json",
            env!("CARGO_MANIFEST_DIR")
        );
        let raw = std::fs::read_to_string(&cases_path)
            .unwrap_or_else(|e| panic!("failed to read {}: {}", cases_path, e));
        let fixture: serde_json::Value =
            serde_json::from_str(&raw).expect("cases.json is valid JSON");

        let cases = fixture["cases"]
            .as_array()
            .expect("cases.json has a `cases` array");
        assert_eq!(cases.len(), 10, "expected exactly 10 conformance cases");

        let mut failures = Vec::new();
        for case in cases {
            let name = case["name"].as_str().unwrap_or("<unnamed>");
            let input = case["input"].as_str().expect("case input is a string");
            let expected: Vec<String> = case["expected"]
                .as_array()
                .expect("case expected is an array")
                .iter()
                .map(|v| v.as_str().expect("expected entry is a string").to_string())
                .collect();

            let actual = split_top_level_statements(input);
            if actual == expected {
                println!("PASS  {}", name);
            } else {
                println!("FAIL  {}", name);
                failures.push(format!(
                    "case {}: expected {:?}, got {:?}",
                    name, expected, actual
                ));
            }
        }

        let passed = cases.len() - failures.len();
        println!("chunker conformance: {}/{} cases passed", passed, cases.len());
        assert!(
            failures.is_empty(),
            "chunker conformance failures:\n{}",
            failures.join("\n")
        );
    }
}
