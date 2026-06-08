// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

use std::fmt;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ErrorKind {
    Unknown,
    Warning,
    NoData,
    Connection,
    NotSupported,
    Data,
    Integrity,
    Auth,
    Transaction,
    Syntax,
    Resource,
    Limit,
    OperatorIntervention,
    System,
    Internal,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RetryScope {
    None,
    Reconnect,
    Statement,
    Transaction,
}

#[derive(Debug)]
pub struct Error {
    pub kind: ErrorKind,
    pub message: String,
    pub sqlstate: Option<String>,
    pub detail: Option<String>,
    pub hint: Option<String>,
}

pub type Result<T> = std::result::Result<T, Error>;

impl Error {
    pub fn new(kind: ErrorKind, message: impl Into<String>) -> Self {
        Self {
            kind,
            message: message.into(),
            sqlstate: None,
            detail: None,
            hint: None,
        }
    }

    pub fn with_sqlstate(
        kind: ErrorKind,
        message: impl Into<String>,
        sqlstate: Option<String>,
        detail: Option<String>,
        hint: Option<String>,
    ) -> Self {
        Self {
            kind,
            message: message.into(),
            sqlstate,
            detail,
            hint,
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if let Some(state) = &self.sqlstate {
            write!(f, "[{}] {}", state, self.message)
        } else {
            write!(f, "{}", self.message)
        }
    }
}

impl std::error::Error for Error {}

impl From<std::io::Error> for Error {
    fn from(err: std::io::Error) -> Self {
        Error::new(ErrorKind::Connection, err.to_string())
    }
}

pub fn error_from_sqlstate(
    sqlstate: &str,
    message: impl Into<String>,
    detail: Option<String>,
    hint: Option<String>,
) -> Error {
    let kind = if sqlstate.len() == 5 {
        match sqlstate {
            "01000" => ErrorKind::Warning,
            "02000" => ErrorKind::NoData,
            "08001" | "08003" | "08004" | "08006" | "08P01" => ErrorKind::Connection,
            "0A000" => ErrorKind::NotSupported,
            "22001" | "22003" | "22007" | "22012" | "22023" | "22P02" | "22P03" => ErrorKind::Data,
            "23000" | "23502" | "23503" | "23505" | "23514" => ErrorKind::Integrity,
            "28000" | "28P01" => ErrorKind::Auth,
            "40001" | "40P01" => ErrorKind::Transaction,
            "42501" | "42601" | "42703" | "42704" | "42710" | "42883" | "42P01" | "42P07" => {
                ErrorKind::Syntax
            }
            "53P00" | "53100" | "53200" | "53300" => ErrorKind::Resource,
            "54000" => ErrorKind::Limit,
            "57014" | "57P01" | "57P03" => ErrorKind::OperatorIntervention,
            "58000" => ErrorKind::System,
            "XX000" => ErrorKind::Internal,
            _ => match &sqlstate[..2] {
                "01" => ErrorKind::Warning,
                "02" => ErrorKind::NoData,
                "08" => ErrorKind::Connection,
                "0A" => ErrorKind::NotSupported,
                "22" => ErrorKind::Data,
                "23" => ErrorKind::Integrity,
                "28" => ErrorKind::Auth,
                "40" => ErrorKind::Transaction,
                "42" => ErrorKind::Syntax,
                "53" => ErrorKind::Resource,
                "54" => ErrorKind::Limit,
                "57" => ErrorKind::OperatorIntervention,
                "58" => ErrorKind::System,
                "XX" => ErrorKind::Internal,
                _ => ErrorKind::Unknown,
            },
        }
    } else {
        ErrorKind::Unknown
    };
    Error::with_sqlstate(kind, message, Some(sqlstate.to_string()), detail, hint)
}

pub fn retry_scope_for_sqlstate(sqlstate: Option<&str>) -> RetryScope {
    // Drivers are fail-closed: fresh statement restart for 40xxx, reconnect
    // only for 08xxx, and no automatic whole-transaction replay.
    let Some(sqlstate) = sqlstate else {
        return RetryScope::None;
    };
    if sqlstate.len() != 5 {
        return RetryScope::None;
    }
    match sqlstate {
        "40001" | "40P01" => RetryScope::Statement,
        _ if &sqlstate[..2] == "08" => RetryScope::Reconnect,
        _ => RetryScope::None,
    }
}

pub fn is_retryable_sqlstate(sqlstate: Option<&str>) -> bool {
    retry_scope_for_sqlstate(sqlstate) != RetryScope::None
}

#[cfg(test)]
mod tests {
    use super::{
        error_from_sqlstate, is_retryable_sqlstate, retry_scope_for_sqlstate, ErrorKind, RetryScope,
    };

    #[test]
    fn exact_sqlstate_mapping_prefers_known_codes() {
        let err = error_from_sqlstate("23505", "duplicate key", None, None);
        assert_eq!(err.kind, ErrorKind::Integrity);
        assert_eq!(err.sqlstate.as_deref(), Some("23505"));
    }

    #[test]
    fn class_fallback_maps_unknown_codes_with_known_prefix() {
        let conn = error_from_sqlstate("08ZZZ", "network issue", None, None);
        assert_eq!(conn.kind, ErrorKind::Connection);

        let data = error_from_sqlstate("22ZZZ", "bad input", None, None);
        assert_eq!(data.kind, ErrorKind::Data);
    }

    #[test]
    fn unknown_or_short_sqlstate_falls_back_to_unknown_kind() {
        let unknown = error_from_sqlstate("ZZZZZ", "unexpected", None, None);
        assert_eq!(unknown.kind, ErrorKind::Unknown);

        let short = error_from_sqlstate("123", "short", None, None);
        assert_eq!(short.kind, ErrorKind::Unknown);
    }

    #[test]
    fn retry_scope_classifies_statement_and_reconnect_boundaries() {
        assert_eq!(
            retry_scope_for_sqlstate(Some("40001")),
            RetryScope::Statement
        );
        assert_eq!(
            retry_scope_for_sqlstate(Some("40P01")),
            RetryScope::Statement
        );
        assert_eq!(
            retry_scope_for_sqlstate(Some("08006")),
            RetryScope::Reconnect
        );
        assert_eq!(retry_scope_for_sqlstate(Some("57014")), RetryScope::None);
        assert_eq!(retry_scope_for_sqlstate(None), RetryScope::None);
    }

    #[test]
    fn retryable_sqlstate_only_covers_fresh_boundary_retries() {
        assert!(is_retryable_sqlstate(Some("40001")));
        assert!(is_retryable_sqlstate(Some("08003")));
        assert!(!is_retryable_sqlstate(Some("57014")));
        assert!(!is_retryable_sqlstate(Some("")));
    }
}
