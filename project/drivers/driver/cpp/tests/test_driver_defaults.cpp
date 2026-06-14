// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "scratchbird/client/connection.h"
#include "scratchbird/client/driver_config.h"
#include "scratchbird/client/network_client.h"

namespace {

class EnvGuard {
public:
    EnvGuard(const char* name, const std::string& value)
        : name_(name) {
        const char* existing = std::getenv(name);
        if (existing) {
            had_value_ = true;
            old_value_ = existing;
        }
#if defined(_WIN32)
        _putenv_s(name, value.c_str());
#else
        setenv(name, value.c_str(), 1);
#endif
    }

    EnvGuard(const char* name)
        : name_(name) {
        const char* existing = std::getenv(name);
        if (existing) {
            had_value_ = true;
            old_value_ = existing;
        }
#if defined(_WIN32)
        _putenv_s(name, "");
#else
        unsetenv(name);
#endif
    }

    ~EnvGuard() {
#if defined(_WIN32)
        if (had_value_) {
            _putenv_s(name_.c_str(), old_value_.c_str());
        } else {
            _putenv_s(name_.c_str(), "");
        }
#else
        if (had_value_) {
            setenv(name_.c_str(), old_value_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }

private:
    std::string name_;
    bool had_value_ = false;
    std::string old_value_;
};

} // namespace

TEST(DriverDefaultsEnvTest, AppliesDefaultsWhenUnset) {
    EnvGuard schema("SCRATCHBIRD_SCHEMA");
    EnvGuard host("SCRATCHBIRD_DRIVER_HOST", "db.example.test");
    EnvGuard port("SCRATCHBIRD_DRIVER_PORT", "4123");
    EnvGuard sslmode("SCRATCHBIRD_DRIVER_SSLMODE", "verify_full");
    EnvGuard timeout("SCRATCHBIRD_DRIVER_CONNECT_TIMEOUT_MS", "12000");
    EnvGuard database("SCRATCHBIRD_DRIVER_DATABASE", "alpha");
    EnvGuard app("SCRATCHBIRD_DRIVER_APPLICATION_NAME", "scratchbird_test");
    EnvGuard cert("SCRATCHBIRD_DRIVER_SSL_CERT", "/tmp/client.crt");
    EnvGuard key("SCRATCHBIRD_DRIVER_SSL_KEY", "/tmp/client.key");
    EnvGuard root("SCRATCHBIRD_DRIVER_SSL_ROOT_CERT", "/tmp/ca.pem");

    scratchbird::client::NetworkClientConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = scratchbird::network::DEFAULT_NATIVE_PORT;
    cfg.application_name = "scratchbird_odbc";

    scratchbird::client::applyDriverDefaultsFromEnv(cfg);

    EXPECT_EQ(cfg.host, "db.example.test");
    EXPECT_EQ(cfg.port, 4123);
    EXPECT_EQ(cfg.ssl_mode, scratchbird::network::SSLMode::VERIFY_FULL);
    EXPECT_EQ(cfg.connect_timeout_ms, 12000u);
    EXPECT_EQ(cfg.database, "alpha");
    EXPECT_EQ(cfg.schema, "users.public");
    EXPECT_EQ(cfg.application_name, "scratchbird_test");
    EXPECT_EQ(cfg.ssl_cert, "/tmp/client.crt");
    EXPECT_EQ(cfg.ssl_key, "/tmp/client.key");
    EXPECT_EQ(cfg.ssl_root_cert, "/tmp/ca.pem");
}

TEST(DriverDefaultsEnvTest, DoesNotOverrideExplicitConfig) {
    EnvGuard schema("SCRATCHBIRD_SCHEMA");
    EnvGuard host("SCRATCHBIRD_DRIVER_HOST", "env.example.test");
    EnvGuard port("SCRATCHBIRD_DRIVER_PORT", "5000");
    EnvGuard app("SCRATCHBIRD_DRIVER_APPLICATION_NAME", "env_app");

    scratchbird::client::NetworkClientConfig cfg;
    cfg.host = "override.host";
    cfg.port = 4100;
    cfg.application_name = "override_app";

    scratchbird::client::applyDriverDefaultsFromEnv(cfg);

    EXPECT_EQ(cfg.host, "override.host");
    EXPECT_EQ(cfg.port, 4100);
    EXPECT_EQ(cfg.application_name, "override_app");
}

TEST(DriverDefaultsEnvTest, DefaultsSchemaWhenConnectionStringOmitsIt) {
    EnvGuard schema("SCRATCHBIRD_SCHEMA");
    scratchbird::client::NetworkClientConfig cfg;
    scratchbird::core::ErrorContext ctx;
    auto status = scratchbird::client::parseDriverConnectionString(
        "scratchbird://admin:pw@127.0.0.1:3092/main?sslmode=disable",
        cfg,
        &ctx);
    ASSERT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    EXPECT_EQ(cfg.schema, "users.public");
}

TEST(DriverDefaultsEnvTest, ParsesManagerProxyConnectionParams) {
    scratchbird::client::NetworkClientConfig cfg;
    scratchbird::core::ErrorContext ctx;
    auto status = scratchbird::client::parseDriverConnectionString(
        "scratchbird://admin:pw@127.0.0.1:3092/main?"
        "front_door_mode=manager_proxy&manager_auth_token=abc123&"
        "manager_username=admin&manager_database=main&"
        "manager_connection_profile=SBsql&manager_client_intent=SBsql&"
        "manager_client_flags=7&manager_auth_fast_path=false",
        cfg,
        &ctx);
    ASSERT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    EXPECT_EQ(cfg.front_door_mode, "manager_proxy");
    EXPECT_EQ(cfg.manager_auth_token, "abc123");
    EXPECT_EQ(cfg.manager_username, "admin");
    EXPECT_EQ(cfg.manager_database, "main");
    EXPECT_EQ(cfg.manager_connection_profile, "SBsql");
    EXPECT_EQ(cfg.manager_client_intent, "SBsql");
    EXPECT_EQ(cfg.manager_client_flags, 7);
    EXPECT_FALSE(cfg.manager_auth_fast_path);
}

TEST(DriverDefaultsEnvTest, ParsesBinaryTransferAndCompressionCompatibilityParams) {
    scratchbird::client::NetworkClientConfig cfg;
    scratchbird::core::ErrorContext ctx;
    auto status = scratchbird::client::parseDriverConnectionString(
        "scratchbird://admin:pw@127.0.0.1:3092/main?"
        "binary_transfer=false&compression=zstd",
        cfg,
        &ctx);
    ASSERT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    EXPECT_FALSE(cfg.binary_transfer);
    EXPECT_TRUE(cfg.enable_compression);
}

TEST(DriverDefaultsEnvTest, RejectsUnsupportedCompressionMode) {
    scratchbird::client::NetworkClientConfig cfg;
    scratchbird::core::ErrorContext ctx;
    auto status = scratchbird::client::parseDriverConnectionString(
        "scratchbird://admin:pw@127.0.0.1:3092/main?compression=gzip",
        cfg,
        &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::INVALID_ARGUMENT);
    EXPECT_NE(ctx.message.find("compression"), std::string::npos);
}

TEST(DriverDefaultsEnvTest, ParsesIpcTransportParams) {
    scratchbird::client::NetworkClientConfig cfg;
    scratchbird::core::ErrorContext ctx;
    auto status = scratchbird::client::parseDriverConnectionString(
        "database=main;transport_mode=local_ipc;ipc_method=unix;ipc_path=build/ipc/scratchbird-main.sock",
        cfg,
        &ctx);
    ASSERT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    EXPECT_EQ(cfg.transport_mode, "local_ipc");
    EXPECT_EQ(cfg.front_door_mode, "direct");
    EXPECT_EQ(cfg.ipc_method, "unix");
    EXPECT_EQ(cfg.ipc_path, "build/ipc/scratchbird-main.sock");
}

TEST(DriverDefaultsEnvTest, ParsesUnixServerEndpointAsLocalIpc) {
    scratchbird::client::NetworkClientConfig cfg;
    scratchbird::core::ErrorContext ctx;
    auto status = scratchbird::client::parseDriverConnectionString(
        "server=unix:/tmp/scratchbird.sock;database=main",
        cfg,
        &ctx);
    ASSERT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    EXPECT_EQ(cfg.transport_mode, "local_ipc");
    EXPECT_EQ(cfg.front_door_mode, "direct");
    EXPECT_EQ(cfg.ipc_method, "unix");
    EXPECT_EQ(cfg.ipc_path, "/tmp/scratchbird.sock");
}

TEST(DriverDefaultsEnvTest, ManagedTransportSetsManagerProxyFrontDoor) {
    scratchbird::client::NetworkClientConfig cfg;
    scratchbird::core::ErrorContext ctx;
    auto status = scratchbird::client::parseDriverConnectionString(
        "database=main;transport_mode=managed",
        cfg,
        &ctx);
    ASSERT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    EXPECT_EQ(cfg.transport_mode, "managed");
    EXPECT_EQ(cfg.front_door_mode, "manager_proxy");
}

TEST(DriverDefaultsEnvTest, ParsesAuthPluginAndPinningParams) {
    scratchbird::client::NetworkClientConfig cfg;
    scratchbird::core::ErrorContext ctx;
    auto status = scratchbird::client::parseDriverConnectionString(
        "database=main;"
        "auth_method_id=scratchbird.auth.proxy_assertion;"
        "auth_token=bearer-token;"
        "auth_method_payload=assertion.jwt;"
        "auth_payload_json=principal_proxy;"
        "auth_payload_b64=cHJveHk=;"
        "auth_provider_profile=corp_ldap_primary;"
        "auth_required_methods=scratchbird.auth.proxy_assertion,scratchbird.auth.factor_chain_2fa;"
        "auth_forbidden_methods=scratchbird.auth.password_compat;"
        "auth_require_channel_binding=true;"
        "workload_identity_token=workload.jwt;"
        "proxy_principal_assertion=proxy.jwt;"
        "connect_client_flags=256",
        cfg,
        &ctx);
    ASSERT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    EXPECT_EQ(cfg.auth_method_id, "scratchbird.auth.proxy_assertion");
    EXPECT_EQ(cfg.auth_token, "bearer-token");
    EXPECT_EQ(cfg.auth_method_payload, "assertion.jwt");
    EXPECT_EQ(cfg.auth_payload_json, "principal_proxy");
    EXPECT_EQ(cfg.auth_payload_b64, "cHJveHk=");
    EXPECT_EQ(cfg.auth_provider_profile, "corp_ldap_primary");
    ASSERT_EQ(cfg.auth_required_methods.size(), 2U);
    EXPECT_EQ(cfg.auth_required_methods[0], "scratchbird.auth.proxy_assertion");
    EXPECT_EQ(cfg.auth_required_methods[1], "scratchbird.auth.factor_chain_2fa");
    ASSERT_EQ(cfg.auth_forbidden_methods.size(), 1U);
    EXPECT_EQ(cfg.auth_forbidden_methods[0], "scratchbird.auth.password_compat");
    EXPECT_TRUE(cfg.auth_require_channel_binding);
    EXPECT_EQ(cfg.workload_identity_token, "workload.jwt");
    EXPECT_EQ(cfg.proxy_principal_assertion, "proxy.jwt");
    EXPECT_EQ(cfg.connect_client_flags, 256);
}

TEST(DriverDefaultsEnvTest, ParseConnectionConfigMirrorsManagedTransportAndSchemaAliases) {
    scratchbird::client::ConnectionConfig cfg;
    scratchbird::core::ErrorContext ctx;
    auto status = scratchbird::client::parseConnectionConfig(
        "scratchbird://admin:pw@127.0.0.1:3092/main?"
        "front_door_mode=manager_proxy&manager_auth_token=abc123&"
        "currentSchema=users.alice&role=ops&applicationName=cpp_api&"
        "sslmode=verify-full&binary_transfer=true&compression=zstd",
        &cfg,
        &ctx);
    ASSERT_EQ(status, scratchbird::core::Status::OK) << ctx.message;
    EXPECT_EQ(cfg.database_name, "main");
    EXPECT_EQ(cfg.username, "admin");
    EXPECT_EQ(cfg.transport_mode, "managed");
    EXPECT_EQ(cfg.front_door_mode, "manager_proxy");
    EXPECT_EQ(cfg.manager_auth_token, "abc123");
    EXPECT_EQ(cfg.current_schema, "users.alice");
    EXPECT_EQ(cfg.role, "ops");
    EXPECT_EQ(cfg.application_name, "cpp_api");
    EXPECT_EQ(cfg.ssl_mode, "verify-full");
    EXPECT_TRUE(cfg.binary_transfer);
    EXPECT_TRUE(cfg.enable_compression);
}

TEST(DriverDefaultsEnvTest, RejectsOverlappingPinningProfiles) {
    scratchbird::client::NetworkClientConfig cfg;
    scratchbird::core::ErrorContext ctx;
    auto status = scratchbird::client::parseDriverConnectionString(
        "database=main;"
        "auth_required_methods=scratchbird.auth.scram_sha_256;"
        "auth_forbidden_methods=scratchbird.auth.scram_sha_256",
        cfg,
        &ctx);
    EXPECT_EQ(status, scratchbird::core::Status::INVALID_ARGUMENT);
    EXPECT_NE(ctx.message.find("required and forbidden"), std::string::npos);
}
