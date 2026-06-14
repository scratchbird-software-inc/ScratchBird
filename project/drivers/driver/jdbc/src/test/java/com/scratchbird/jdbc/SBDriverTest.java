// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird JDBC Driver tests
 */
package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.sql.SQLException;
import java.util.Properties;
import org.junit.jupiter.api.Test;

public class SBDriverTest {

    @Test
    public void parsesBasicUrl() throws SQLException {
        SBConnectionProperties props =
            SBDriver.parseURL("jdbc:scratchbird://localhost:3092/demo", null);
        assertEquals("localhost", props.getHost());
        assertEquals(3092, props.getPort());
        assertEquals("demo", props.getDatabase());
        assertNull(props.getCurrentSchema());
        assertNull(props.toProperties().getProperty("currentSchema"));
    }

    @Test
    public void parsesUrlWithParams() throws SQLException {
        Properties info = new Properties();
        info.setProperty("user", "alice");
        SBConnectionProperties props =
            SBDriver.parseURL("jdbc:scratchbird://db.example.com:3093/app?sslmode=require&connectTimeout=15&metadataExpandSchemaParents=true",
                info);
        assertEquals("db.example.com", props.getHost());
        assertEquals(3093, props.getPort());
        assertEquals("app", props.getDatabase());
        assertEquals("alice", props.getUser());
        assertEquals("require", props.getSslMode());
        assertEquals("15", props.getProperty("connectTimeout"));
        assertEquals("true", props.getProperty("metadataExpandSchemaParents"));
    }

    @Test
    public void parsesIpv6Url() throws SQLException {
        SBConnectionProperties props =
            SBDriver.parseURL("jdbc:scratchbird://[::1]:3092/testdb", null);
        assertEquals("::1", props.getHost());
        assertEquals(3092, props.getPort());
        assertEquals("testdb", props.getDatabase());
        assertNotNull(props);
    }

    @Test
    public void defaultsEmptyPortSegmentFromDbeaverTemplate() throws SQLException {
        SBConnectionProperties props =
            SBDriver.parseURL("jdbc:scratchbird://127.0.0.1:/default?sslmode=require", null);
        assertEquals("127.0.0.1", props.getHost());
        assertEquals(3092, props.getPort());
        assertEquals("default", props.getDatabase());
    }

    @Test
    public void defaultsEmptyIpv6PortSegmentFromDbeaverTemplate() throws SQLException {
        SBConnectionProperties props =
            SBDriver.parseURL("jdbc:scratchbird://[::1]:/default?sslmode=require", null);
        assertEquals("::1", props.getHost());
        assertEquals(3092, props.getPort());
        assertEquals("default", props.getDatabase());
    }

    @Test
    public void parsesCompressionAndRejectsUnsupportedAlgorithms() throws SQLException {
        SBConnectionProperties zstd = SBDriver.parseURL(
            "jdbc:scratchbird://localhost:3092/demo?compression=zstd", null);
        assertEquals("zstd", zstd.getCompression());

        SBConnectionProperties off = SBDriver.parseURL(
            "jdbc:scratchbird://localhost:3092/demo?compression=none", null);
        assertEquals("off", off.getCompression());

        assertThrows(SQLException.class, () ->
            SBDriver.parseURL("jdbc:scratchbird://localhost:3092/demo?compression=gzip", null));
    }

    @Test
    public void parsesDbeaverSchemaExpansionAlias() throws SQLException {
        SBConnectionProperties props = SBDriver.parseURL(
            "jdbc:scratchbird://localhost:3092/demo?dbeaver_expand_schema_parents=true", null);
        assertEquals("true", props.getProperty("metadataExpandSchemaParents"));
    }

    @Test
    public void preservesExplicitCurrentSchemaOverride() throws SQLException {
        SBConnectionProperties props = SBDriver.parseURL(
            "jdbc:scratchbird://localhost:3092/demo?currentSchema=users.public", null);
        assertEquals("users.public", props.getCurrentSchema());
        assertEquals("users.public", props.toProperties().getProperty("currentSchema"));
    }

    @Test
    public void normalizesCanonicalDsnAliasesUsedByDbeaverAdapter() throws SQLException {
        SBConnectionProperties props = SBDriver.parseURL(
            "jdbc:scratchbird://localhost:3092/demo" +
                "?connect_timeout=11" +
                "&socket_timeout=22" +
                "&search_path=users.public" +
                "&application_name=dbeaver" +
                "&binary_transfer=false" +
                "&fetch_size=64" +
                "&acquire_timeout=17" +
                "&managerAuthToken=manager-token" +
                "&managerClientFlags=259" +
                "&managerAuthFastPath=false",
            null);

        assertEquals("11", props.getProperty("connectTimeout"));
        assertEquals("11", props.getProperty("connect_timeout"));
        assertEquals("22", props.getProperty("socketTimeout"));
        assertEquals("22", props.getProperty("socket_timeout"));
        assertEquals("users.public", props.getCurrentSchema());
        assertEquals("users.public", props.getProperty("search_path"));
        assertEquals("dbeaver", props.getProperty("application_name"));
        assertEquals("false", props.getProperty("binary_transfer"));
        assertEquals("64", props.getProperty("fetch_size"));
        assertEquals("17", props.getProperty("acquire_timeout"));
        assertEquals("manager-token", props.getProperty("manager_auth_token"));
        assertEquals("259", props.getProperty("manager_client_flags"));
        assertEquals("false", props.getProperty("manager_auth_fast_path"));
    }

    @Test
    public void ignoresBlankOptionalPropertiesFromDbeaverForms() throws SQLException {
        Properties info = new Properties();
        info.setProperty("port", "");
        info.setProperty("connect_timeout", "");
        info.setProperty("socket_timeout", "");
        info.setProperty("login_timeout", "");
        info.setProperty("fetch_size", "");
        info.setProperty("prepareThreshold", "");
        info.setProperty("min_pool_size", "");
        info.setProperty("max_pool_size", "");
        info.setProperty("connection_lifetime", "");
        info.setProperty("acquire_timeout", "");
        info.setProperty("manager_client_flags", "");
        info.setProperty("connect_client_flags", "");
        info.setProperty("binary_transfer", "");
        info.setProperty("pooling", "");
        info.setProperty("metadataExpandSchemaParents", "");

        SBConnectionProperties props = SBDriver.parseURL(
            "jdbc:scratchbird://127.0.0.1:3092/default?sslmode=require",
            info);

        assertEquals(3092, props.getPort());
        assertEquals("30", props.getProperty("connect_timeout"));
        assertEquals("0", props.getProperty("socket_timeout"));
        assertEquals("30", props.getProperty("login_timeout"));
        assertEquals("0", props.getProperty("fetch_size"));
        assertEquals("5", props.getProperty("prepareThreshold"));
        assertEquals("0", props.getProperty("min_pool_size"));
        assertEquals("10", props.getProperty("max_pool_size"));
        assertEquals("30", props.getProperty("connection_lifetime"));
        assertEquals("30", props.getProperty("acquire_timeout"));
        assertEquals("0", props.getProperty("manager_client_flags"));
        assertEquals("256", props.getProperty("connect_client_flags"));
        assertEquals("true", props.getProperty("binary_transfer"));
        assertEquals("true", props.getProperty("pooling"));
        assertEquals("false", props.getProperty("metadataExpandSchemaParents"));
    }

    @Test
    public void parsesAuthPluginAndPinningParams() throws SQLException {
        SBConnectionProperties props = SBDriver.parseURL(
            "jdbc:scratchbird://localhost:3092/demo" +
                "?connect_client_flags=257" +
                "&auth_token=bearer-token" +
                "&auth_method_id=scratchbird.auth.proxy_assertion" +
                "&auth_method_payload=opaque" +
                "&auth_payload_json=%7B%22subject%22%3A%22alice%22%7D" +
                "&auth_payload_b64=YWJj" +
                "&auth_provider_profile=corp_primary" +
                "&auth_required_methods=SCRAM_SHA_256%2CTOKEN" +
                "&auth_forbidden_methods=MD5" +
                "&auth_require_channel_binding=true" +
                "&workload_identity_token=jwt-token" +
                "&proxy_principal_assertion=signed-assertion",
            null);

        assertEquals("257", props.getProperty("connect_client_flags"));
        assertEquals("bearer-token", props.getProperty("auth_token"));
        assertEquals("scratchbird.auth.proxy_assertion", props.getProperty("auth_method_id"));
        assertEquals("opaque", props.getProperty("auth_method_payload"));
        assertEquals("{\"subject\":\"alice\"}", props.getProperty("auth_payload_json"));
        assertEquals("YWJj", props.getProperty("auth_payload_b64"));
        assertEquals("corp_primary", props.getProperty("auth_provider_profile"));
        assertEquals("SCRAM_SHA_256,TOKEN", props.getProperty("auth_required_methods"));
        assertEquals("MD5", props.getProperty("auth_forbidden_methods"));
        assertEquals("true", props.getProperty("auth_require_channel_binding"));
        assertEquals("jwt-token", props.getProperty("workload_identity_token"));
        assertEquals("signed-assertion", props.getProperty("proxy_principal_assertion"));
    }

    @Test
    public void rejectsInvalidAuthMethodNamespace() {
        SQLException ex = assertThrows(SQLException.class, () ->
            SBDriver.parseURL(
                "jdbc:scratchbird://localhost:3092/demo?auth_method_id=invalid.namespace",
                null));
        assertEquals("0A000", ex.getSQLState());
    }
}
