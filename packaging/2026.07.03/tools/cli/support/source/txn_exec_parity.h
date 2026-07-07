// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "scratchbird/core/error_context.h"
#include "scratchbird/core/status.h"

namespace scratchbird::cli::parity {

struct ExecObservation {
    int64_t rows_affected{0};
    int64_t rows_returned{0};
};

class TxnExecClient {
public:
    virtual ~TxnExecClient() = default;

    virtual core::Status executeStatement(const std::string& sql,
                                          ExecObservation* observation,
                                          core::ErrorContext* ctx) = 0;
    virtual core::Status beginTransaction(core::ErrorContext* ctx) = 0;
    virtual core::Status commit(core::ErrorContext* ctx) = 0;
    virtual core::Status rollback(core::ErrorContext* ctx) = 0;
    virtual std::string lastError() const = 0;
};

bool supportsPreparedTransactions();
bool supportsDormantReattach();
bool supportsPortalResume();
core::Status buildPreparedTransactionSql(const std::string& verb,
                                         const std::string& global_transaction_id,
                                         std::string* sql,
                                         core::ErrorContext* ctx);
core::Status rejectDormantReattach(const char* operation,
                                   core::ErrorContext* ctx);

// Execute a non-prepare statement and validate optional expectations:
// - expect_rows_affected
// - expect_rows
void runNativeExecCase(TxnExecClient& client,
                       const nlohmann::json& test,
                       nlohmann::json& result,
                       bool* had_error,
                       core::ErrorContext* ctx);

// Execute transaction flow:
// begin -> sql -> (commit|rollback) -> optional verify_sql.
// Optional fields:
// - txn_end: "commit" (default) or "rollback"
// - expect_rows_affected
// - savepoint_name
// - rollback_to_savepoint (bool, requires savepoint_name)
// - release_savepoint (bool, requires savepoint_name)
// - verify_sql
// - verify_expect_rows (falls back to expect_rows if omitted)
// - cleanup_sql
void runTxnExecCase(TxnExecClient& client,
                    const nlohmann::json& test,
                    nlohmann::json& result,
                    bool* had_error,
                    core::ErrorContext* ctx);

}  // namespace scratchbird::cli::parity
