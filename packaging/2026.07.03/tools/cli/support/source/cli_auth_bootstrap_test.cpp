// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "cli_auth_bootstrap.h"
#include "scratchbird/client/connection.h"

namespace {

bool expect(bool condition, const std::string& message, int* failures) {
    if (condition) {
        return true;
    }
    std::cerr << "FAIL: " << message << "\n";
    if (failures != nullptr) {
        ++(*failures);
    }
    return false;
}

void testBuildConnectionTargetIncludesAuthToken(int* failures) {
    scratchbird::cli::ConnectionBootstrapOptions options;
    options.database_path = "testdb";
    options.host = "127.0.0.1";
    options.port = 3092;
    options.auth_method_id = "scratchbird.auth.token";
    options.auth_token = "token_value";
    options.manager_auth_token = "manager_token";
    options.front_door_mode = "manager_proxy";
    options.manager_connection_profile = "SBsql";

    const std::string target = scratchbird::cli::buildConnectionTarget(options);
    expect(target.find("auth_token=token_value") != std::string::npos,
           "auth_token should be present in CLI-built target",
           failures);
    expect(target.find("auth_method_id=scratchbird.auth.token") != std::string::npos,
           "auth_method_id should be present in CLI-built target",
           failures);
    expect(target.find("front_door_mode=manager_proxy") != std::string::npos,
           "front_door_mode should be present in CLI-built target",
           failures);
    expect(target.find("manager_auth_token=manager_token") != std::string::npos,
           "manager_auth_token should be present in CLI-built target",
           failures);
}

void testBuildConnectionTargetPreservesExplicitConnectionString(int* failures) {
    scratchbird::cli::ConnectionBootstrapOptions options;
    options.connection_string =
        "database=testdb;protocol=native;transport_mode=inet_listener;auth_token=abc";

    const std::string target = scratchbird::cli::buildConnectionTarget(options);
    expect(target == options.connection_string,
           "explicit connection string should pass through unchanged",
           failures);
}

void testBuildConnectionTargetIncludesLocalIpc(int* failures) {
    scratchbird::cli::ConnectionBootstrapOptions options;
    options.database_path = "testdb";
    options.mode = "local-ipc";
    options.ipc_method = "unix";
    options.ipc_path = "/tmp/scratchbird.sbps.sock";

    const std::string target = scratchbird::cli::buildConnectionTarget(options);
    expect(target.find("transport_mode=local_ipc") != std::string::npos,
           "local IPC target should select local_ipc transport",
           failures);
    expect(target.find("ipc_method=unix") != std::string::npos,
           "local IPC target should include ipc_method",
           failures);
    expect(target.find("ipc_path=/tmp/scratchbird.sbps.sock") != std::string::npos,
           "local IPC target should include ipc_path",
           failures);
    expect(target.find("host=") == std::string::npos,
           "local IPC target should not include inet host",
           failures);
    expect(target.find("port=") == std::string::npos,
           "local IPC target should not include inet port",
           failures);
}

void testProbeAuthSurfaceRejectsInvalidNamespaceBeforeDial(int* failures) {
    scratchbird::client::AuthProbeResult probe;
    scratchbird::core::ErrorContext ctx;
    const std::string target =
        "database=testdb;protocol=native;transport_mode=inet_listener;host=127.0.0.1;port=3092;"
        "auth_method_id=invalid.namespace";
    const auto status = scratchbird::client::probeAuthSurface(target, &probe, &ctx);
    expect(status != scratchbird::core::Status::OK,
           "invalid auth method namespace should fail locally",
           failures);
    expect(ctx.message.find("auth_method_id must start with scratchbird.auth.") != std::string::npos,
           "invalid auth method namespace should explain failure",
           failures);
}

void testRenderResolvedAuthContextText(int* failures) {
    scratchbird::client::ResolvedAuthContext context;
    context.ingress_mode = "manager_proxy";
    context.resolved_auth_method = "TOKEN";
    context.resolved_auth_plugin_id = "scratchbird.auth.token";
    context.manager_authenticated = true;
    context.attached = true;

    const std::string rendered = scratchbird::cli::renderResolvedAuthContextText(context);
    expect(rendered.find("ingress_mode=manager_proxy") != std::string::npos,
           "resolved auth context should render ingress mode",
           failures);
    expect(rendered.find("resolved_auth_method=TOKEN") != std::string::npos,
           "resolved auth context should render auth method",
           failures);
    expect(rendered.find("attached=true") != std::string::npos,
           "resolved auth context should render attached flag",
           failures);
}

} // namespace

int main() {
    int failures = 0;
    testBuildConnectionTargetIncludesAuthToken(&failures);
    testBuildConnectionTargetPreservesExplicitConnectionString(&failures);
    testBuildConnectionTargetIncludesLocalIpc(&failures);
    testProbeAuthSurfaceRejectsInvalidNamespaceBeforeDial(&failures);
    testRenderResolvedAuthContextText(&failures);

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "cli_auth_bootstrap_test: PASS\n";
    return 0;
}
