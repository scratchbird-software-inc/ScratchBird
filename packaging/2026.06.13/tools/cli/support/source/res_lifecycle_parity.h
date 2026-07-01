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

struct LifecycleObservation {
    int64_t rows_affected{0};
    int64_t rows_returned{0};
};

struct AdminLifecycleRoute {
    std::string operation;
    std::string management_operation_key;
    std::string client_statement;
    std::string required_right;
    bool mutates_lifecycle{false};
    bool requires_explicit_policy_evidence{false};
};

class ResourceLifecycleClient {
public:
    virtual ~ResourceLifecycleClient() = default;

    virtual core::Status connect(core::ErrorContext* ctx) = 0;
    virtual core::Status executeStatement(const std::string& sql,
                                          LifecycleObservation* observation,
                                          core::ErrorContext* ctx) = 0;
    virtual void disconnect() = 0;
    virtual std::string lastError() const = 0;
};

// Execute repeated connect/execute/disconnect cycles with cleanup guarantees.
// Required fields:
// - sql
// Optional fields:
// - loop_iterations (default: 1)
// - expect_total_rows_affected
// - expect_total_rows
void runResourceLifecycleLoopCase(ResourceLifecycleClient& client,
                                  const nlohmann::json& test,
                                  nlohmann::json& result,
                                  bool* had_error,
                                  core::ErrorContext* ctx);

const std::vector<AdminLifecycleRoute>& adminLifecycleRoutes();
bool hasCompleteAdminLifecycleRouteCoverage(std::vector<std::string>* missing_operations);

}  // namespace scratchbird::cli::parity
