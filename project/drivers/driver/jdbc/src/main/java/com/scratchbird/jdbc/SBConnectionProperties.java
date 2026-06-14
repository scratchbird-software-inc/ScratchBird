// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird JDBC Driver
 * Copyright (c) 2025 ScratchBird Project
 */
package com.scratchbird.jdbc;

import java.util.Properties;

/**
 * Connection properties for ScratchBird JDBC connections.
 *
 * <p>This class holds all configuration options for establishing a connection
 * to a ScratchBird database server.</p>
 */
public class SBConnectionProperties {

    // Connection target
    private String host = "localhost";
    private int port = SBDriver.DEFAULT_PORT;
    private String frontDoorMode = "direct";
    private String protocol = "native";
    private String database;

    // Authentication
    private String user;
    private String password;

    // SSL/TLS
    private String ssl = "require";
    private String sslMode;
    private String sslCert;
    private String sslKey;
    private String sslRootCert;
    private String sslPassword;

    // Timeouts (seconds)
    private int connectTimeout = 30;
    private int socketTimeout = 0;
    private int loginTimeout = 30;

    // Connection options
    private boolean tcpKeepAlive = true;
    private String currentSchema;
    private boolean metadataExpandSchemaParents = false;
    private String metadataFixtureCatalog = "";
    private String role;
    private String applicationName;
    private boolean readOnly = false;
    private boolean autoCommit = true;

    // Performance options
    private boolean pooling = true;
    private int minPoolSize = 0;
    private int maxPoolSize = 10;
    private int connectionLifetime = 30;
    private int acquireTimeout = 30;
    private int defaultRowFetchSize = 0;
    private int prepareThreshold = 5;
    private boolean binaryTransfer = true;
    private String compression = "off";
    private String managerAuthToken = "";
    private String managerUsername = "";
    private String managerDatabase = "";
    private String managerConnectionProfile = "SBsql";
    private String managerClientIntent = "SBsql";
    private int managerClientFlags = 0;
    private boolean managerAuthFastPath = true;
    private int connectClientFlags = 0x0100;
    private String authToken;
    private String authMethodId;
    private String authMethodPayload;
    private String authPayloadJson;
    private String authPayloadB64;
    private String authProviderProfile;
    private String authRequiredMethods;
    private String authForbiddenMethods;
    private boolean authRequireChannelBinding = false;
    private String workloadIdentityToken;
    private String proxyPrincipalAssertion;
    private boolean reWriteBatchedInserts = false;

    // Logging
    private String loggerLevel = "OFF";
    private String loggerFile;

    // Additional properties
    private Properties extraProperties = new Properties();

    /**
     * Default constructor.
     */
    public SBConnectionProperties() {
    }

    /**
     * Constructs properties from a Properties object.
     *
     * @param props source properties
     */
    public SBConnectionProperties(Properties props) {
        if (props != null) {
            for (String key : props.stringPropertyNames()) {
                setProperty(key, props.getProperty(key));
            }
        }
    }

    /**
     * Sets a property by name.
     *
     * @param key property name
     * @param value property value
     */
    public void setProperty(String key, String value) {
        if (key == null || value == null) {
            return;
        }

        switch (key.toLowerCase()) {
            case "host":
            case "servername":
            case "pghost":
                this.host = value;
                break;
            case "port":
            case "portnumber":
            case "pgport":
                this.port = parseOptionalInt(value, this.port);
                break;
            case "front_door_mode":
            case "frontdoormode":
            case "connection_mode":
            case "ingress_mode":
                this.frontDoorMode = normalizeFrontDoorMode(value);
                break;
            case "database":
            case "databasename":
            case "dbname":
            case "pgdatabase":
                this.database = value;
                break;
            case "protocol":
            case "parser":
            case "dialect":
                this.protocol = normalizeNativeProtocol(value);
                break;
            case "user":
            case "username":
            case "pguser":
                this.user = value;
                break;
            case "password":
            case "pgpassword":
                this.password = value;
                break;
            case "ssl":
                this.ssl = value;
                break;
            case "sslmode":
                this.sslMode = value;
                if (this.ssl == null || "prefer".equals(this.ssl)) {
                    this.ssl = value;
                }
                break;
            case "sslcert":
                this.sslCert = value;
                break;
            case "sslkey":
                this.sslKey = value;
                break;
            case "sslrootcert":
                this.sslRootCert = value;
                break;
            case "sslpassword":
                this.sslPassword = value;
                break;
            case "connecttimeout":
            case "connect_timeout":
                this.connectTimeout = parseOptionalInt(value, this.connectTimeout);
                break;
            case "sockettimeout":
            case "socket_timeout":
                this.socketTimeout = parseOptionalInt(value, this.socketTimeout);
                break;
            case "logintimeout":
            case "login_timeout":
                this.loginTimeout = parseOptionalInt(value, this.loginTimeout);
                break;
            case "tcpkeepalive":
            case "tcp_keep_alive":
                this.tcpKeepAlive = parseOptionalBoolean(value, this.tcpKeepAlive);
                break;
            case "schema":
            case "currentschema":
            case "search_path":
            case "searchpath":
                this.currentSchema = normalizeOptionalText(value);
                break;
            case "metadataexpandschemaparents":
            case "metadata_expand_schema_parents":
            case "expandschemaparents":
            case "expand_schema_parents":
            case "dbeaverexpandschemaparents":
            case "dbeaver_expand_schema_parents":
                this.metadataExpandSchemaParents = parseOptionalBoolean(value, this.metadataExpandSchemaParents);
                break;
            case "metadatafixturecatalog":
            case "metadata_fixture_catalog":
                this.metadataFixtureCatalog = normalizeOptionalText(value);
                if (this.metadataFixtureCatalog == null) {
                    this.metadataFixtureCatalog = "";
                }
                break;
            case "role":
                this.role = value;
                break;
            case "applicationname":
            case "application_name":
                this.applicationName = value;
                break;
            case "readonly":
                this.readOnly = parseOptionalBoolean(value, this.readOnly);
                break;
            case "autocommit":
                this.autoCommit = parseOptionalBoolean(value, this.autoCommit);
                break;
            case "defaultrowfetchsize":
            case "fetchsize":
            case "fetch_size":
            case "default_fetch_size":
                this.defaultRowFetchSize = parseOptionalInt(value, this.defaultRowFetchSize);
                break;
            case "preparethreshold":
                this.prepareThreshold = parseOptionalInt(value, this.prepareThreshold);
                break;
            case "binarytransfer":
            case "binary_transfer":
                this.binaryTransfer = parseOptionalBoolean(value, this.binaryTransfer);
                break;
            case "pooling":
                this.pooling = parseOptionalBoolean(value, this.pooling);
                break;
            case "minpoolsize":
            case "min_pool_size":
                this.minPoolSize = parseOptionalInt(value, this.minPoolSize);
                break;
            case "maxpoolsize":
            case "max_pool_size":
                this.maxPoolSize = parseOptionalInt(value, this.maxPoolSize);
                break;
            case "connectionlifetime":
            case "connection_lifetime":
            case "poolingconnectionlifetime":
                this.connectionLifetime = parseOptionalInt(value, this.connectionLifetime);
                break;
            case "acquiretimeout":
            case "acquire_timeout":
            case "poolingacquiretimeout":
                this.acquireTimeout = parseOptionalInt(value, this.acquireTimeout);
                break;
            case "compression":
                this.compression = normalizeCompression(value);
                break;
            case "manager_auth_token":
            case "managerauthtoken":
            case "mcp_auth_token":
                this.managerAuthToken = value;
                break;
            case "manager_username":
            case "managerusername":
            case "mcp_username":
                this.managerUsername = value;
                break;
            case "manager_database":
            case "managerdatabase":
            case "mcp_database":
                this.managerDatabase = value;
                break;
            case "manager_connection_profile":
            case "managerconnectionprofile":
            case "mcp_connection_profile":
                this.managerConnectionProfile = value;
                break;
            case "manager_client_intent":
            case "managerclientintent":
            case "mcp_client_intent":
                this.managerClientIntent = value;
                break;
            case "manager_client_flags":
            case "managerclientflags":
            case "mcp_client_flags":
                this.managerClientFlags = parseOptionalInt(value, this.managerClientFlags);
                break;
            case "manager_auth_fast_path":
            case "managerauthfastpath":
            case "mcp_auth_fast_path":
                this.managerAuthFastPath =
                    "1".equals(value) ||
                    "true".equalsIgnoreCase(value) ||
                    "yes".equalsIgnoreCase(value) ||
                    "on".equalsIgnoreCase(value);
                break;
            case "client_flags":
            case "connect_client_flags":
                this.connectClientFlags = parseOptionalInt(value, this.connectClientFlags);
                break;
            case "auth_token":
            case "authtoken":
            case "bearer_token":
            case "bearertoken":
            case "token":
                this.authToken = value;
                break;
            case "auth_method_id":
            case "authmethodid":
                if (!value.trim().isEmpty() && !value.startsWith("scratchbird.auth.")) {
                    throw new IllegalArgumentException("invalid auth_method_id namespace");
                }
                this.authMethodId = value.trim();
                break;
            case "auth_method_payload":
            case "authmethodpayload":
                this.authMethodPayload = value;
                break;
            case "auth_payload_json":
            case "authpayloadjson":
                this.authPayloadJson = value;
                break;
            case "auth_payload_b64":
            case "authpayloadb64":
                this.authPayloadB64 = value;
                break;
            case "auth_provider_profile":
            case "authproviderprofile":
                this.authProviderProfile = value;
                break;
            case "auth_required_methods":
            case "authrequiredmethods":
                this.authRequiredMethods = value;
                break;
            case "auth_forbidden_methods":
            case "authforbiddenmethods":
                this.authForbiddenMethods = value;
                break;
            case "auth_require_channel_binding":
            case "authrequirechannelbinding":
                this.authRequireChannelBinding =
                    "1".equals(value) ||
                    "true".equalsIgnoreCase(value) ||
                    "yes".equalsIgnoreCase(value) ||
                    "on".equalsIgnoreCase(value);
                break;
            case "workload_identity_token":
            case "workloadidentitytoken":
                this.workloadIdentityToken = value;
                break;
            case "proxy_principal_assertion":
            case "proxyprincipalassertion":
            case "proxy_assertion":
                this.proxyPrincipalAssertion = value;
                break;
            case "rewritebatchedinserts":
            case "rewrite_batched_inserts":
                this.reWriteBatchedInserts = parseOptionalBoolean(value, this.reWriteBatchedInserts);
                break;
            case "loggerlevel":
            case "loglevel":
                this.loggerLevel = value.toUpperCase();
                break;
            case "loggerfile":
            case "logfile":
                this.loggerFile = value;
                break;
            default:
                extraProperties.setProperty(key, value);
                break;
        }
    }

    /**
     * Gets a property by name.
     *
     * @param key property name
     * @return property value or null
     */
    public String getProperty(String key) {
        if (key == null) {
            return null;
        }

        switch (key.toLowerCase()) {
            case "host":
            case "servername":
                return host;
            case "port":
            case "portnumber":
                return String.valueOf(port);
            case "front_door_mode":
            case "frontdoormode":
            case "connection_mode":
            case "ingress_mode":
                return frontDoorMode;
            case "database":
            case "databasename":
            case "dbname":
                return database;
            case "protocol":
            case "parser":
            case "dialect":
                return protocol;
            case "user":
            case "username":
                return user;
            case "password":
                return password;
            case "ssl":
                return ssl;
            case "sslmode":
                return sslMode != null ? sslMode : ssl;
            case "sslcert":
                return sslCert;
            case "sslkey":
                return sslKey;
            case "sslrootcert":
                return sslRootCert;
            case "sslpassword":
                return sslPassword;
            case "connecttimeout":
            case "connect_timeout":
                return String.valueOf(connectTimeout);
            case "sockettimeout":
            case "socket_timeout":
                return String.valueOf(socketTimeout);
            case "logintimeout":
            case "login_timeout":
                return String.valueOf(loginTimeout);
            case "tcpkeepalive":
            case "tcp_keep_alive":
                return String.valueOf(tcpKeepAlive);
            case "schema":
            case "currentschema":
            case "search_path":
            case "searchpath":
                return currentSchema;
            case "metadataexpandschemaparents":
            case "metadata_expand_schema_parents":
            case "expandschemaparents":
            case "expand_schema_parents":
            case "dbeaverexpandschemaparents":
            case "dbeaver_expand_schema_parents":
                return String.valueOf(metadataExpandSchemaParents);
            case "metadatafixturecatalog":
            case "metadata_fixture_catalog":
                return metadataFixtureCatalog;
            case "role":
                return role;
            case "applicationname":
            case "application_name":
                return applicationName;
            case "readonly":
                return String.valueOf(readOnly);
            case "autocommit":
                return String.valueOf(autoCommit);
            case "defaultrowfetchsize":
            case "fetchsize":
            case "fetch_size":
            case "default_fetch_size":
                return String.valueOf(defaultRowFetchSize);
            case "preparethreshold":
                return String.valueOf(prepareThreshold);
            case "binarytransfer":
            case "binary_transfer":
                return String.valueOf(binaryTransfer);
            case "pooling":
                return String.valueOf(pooling);
            case "minpoolsize":
            case "min_pool_size":
                return String.valueOf(minPoolSize);
            case "maxpoolsize":
            case "max_pool_size":
                return String.valueOf(maxPoolSize);
            case "connectionlifetime":
            case "connection_lifetime":
            case "poolingconnectionlifetime":
                return String.valueOf(connectionLifetime);
            case "acquiretimeout":
            case "acquire_timeout":
            case "poolingacquiretimeout":
                return String.valueOf(acquireTimeout);
            case "compression":
                return compression;
            case "manager_auth_token":
            case "managerauthtoken":
            case "mcp_auth_token":
                return managerAuthToken;
            case "manager_username":
            case "managerusername":
            case "mcp_username":
                return managerUsername;
            case "manager_database":
            case "managerdatabase":
            case "mcp_database":
                return managerDatabase;
            case "manager_connection_profile":
            case "managerconnectionprofile":
            case "mcp_connection_profile":
                return managerConnectionProfile;
            case "manager_client_intent":
            case "managerclientintent":
            case "mcp_client_intent":
                return managerClientIntent;
            case "manager_client_flags":
            case "managerclientflags":
            case "mcp_client_flags":
                return String.valueOf(managerClientFlags);
            case "manager_auth_fast_path":
            case "managerauthfastpath":
            case "mcp_auth_fast_path":
                return String.valueOf(managerAuthFastPath);
            case "client_flags":
            case "connect_client_flags":
                return String.valueOf(connectClientFlags);
            case "auth_token":
            case "authtoken":
            case "bearer_token":
            case "bearertoken":
            case "token":
                return authToken;
            case "auth_method_id":
            case "authmethodid":
                return authMethodId;
            case "auth_method_payload":
            case "authmethodpayload":
                return authMethodPayload;
            case "auth_payload_json":
            case "authpayloadjson":
                return authPayloadJson;
            case "auth_payload_b64":
            case "authpayloadb64":
                return authPayloadB64;
            case "auth_provider_profile":
            case "authproviderprofile":
                return authProviderProfile;
            case "auth_required_methods":
            case "authrequiredmethods":
                return authRequiredMethods;
            case "auth_forbidden_methods":
            case "authforbiddenmethods":
                return authForbiddenMethods;
            case "auth_require_channel_binding":
            case "authrequirechannelbinding":
                return String.valueOf(authRequireChannelBinding);
            case "workload_identity_token":
            case "workloadidentitytoken":
                return workloadIdentityToken;
            case "proxy_principal_assertion":
            case "proxyprincipalassertion":
            case "proxy_assertion":
                return proxyPrincipalAssertion;
            case "rewritebatchedinserts":
            case "rewrite_batched_inserts":
                return String.valueOf(reWriteBatchedInserts);
            case "loggerlevel":
                return loggerLevel;
            case "loggerfile":
                return loggerFile;
            default:
                return extraProperties.getProperty(key);
        }
    }

    // Getters and setters

    public String getHost() {
        return host;
    }

    public void setHost(String host) {
        this.host = host;
    }

    public int getPort() {
        return port;
    }

    public void setPort(int port) {
        this.port = port;
    }

    public String getFrontDoorMode() {
        return frontDoorMode;
    }

    public void setFrontDoorMode(String frontDoorMode) {
        this.frontDoorMode = normalizeFrontDoorMode(frontDoorMode);
    }

    public String getDatabase() {
        return database;
    }

    public void setDatabase(String database) {
        this.database = database;
    }

    public String getProtocol() {
        return protocol;
    }

    public void setProtocol(String protocol) {
        this.protocol = normalizeNativeProtocol(protocol);
    }

    public String getUser() {
        return user;
    }

    public void setUser(String user) {
        this.user = user;
    }

    public String getPassword() {
        return password;
    }

    public void setPassword(String password) {
        this.password = password;
    }

    public String getSsl() {
        return ssl;
    }

    public void setSsl(String ssl) {
        this.ssl = ssl;
    }

    public String getSslMode() {
        return sslMode != null ? sslMode : ssl;
    }

    public void setSslMode(String sslMode) {
        this.sslMode = sslMode;
    }

    public String getSslCert() {
        return sslCert;
    }

    public void setSslCert(String sslCert) {
        this.sslCert = sslCert;
    }

    public String getSslKey() {
        return sslKey;
    }

    public void setSslKey(String sslKey) {
        this.sslKey = sslKey;
    }

    public String getSslRootCert() {
        return sslRootCert;
    }

    public void setSslRootCert(String sslRootCert) {
        this.sslRootCert = sslRootCert;
    }

    public String getSslPassword() {
        return sslPassword;
    }

    public void setSslPassword(String sslPassword) {
        this.sslPassword = sslPassword;
    }

    public int getConnectTimeout() {
        return connectTimeout;
    }

    public void setConnectTimeout(int connectTimeout) {
        this.connectTimeout = connectTimeout;
    }

    public int getSocketTimeout() {
        return socketTimeout;
    }

    public void setSocketTimeout(int socketTimeout) {
        this.socketTimeout = socketTimeout;
    }

    public int getLoginTimeout() {
        return loginTimeout;
    }

    public void setLoginTimeout(int loginTimeout) {
        this.loginTimeout = loginTimeout;
    }

    public boolean isTcpKeepAlive() {
        return tcpKeepAlive;
    }

    public void setTcpKeepAlive(boolean tcpKeepAlive) {
        this.tcpKeepAlive = tcpKeepAlive;
    }

    public String getCurrentSchema() {
        return currentSchema;
    }

    public void setCurrentSchema(String currentSchema) {
        this.currentSchema = normalizeOptionalText(currentSchema);
    }

    public boolean isMetadataExpandSchemaParents() {
        return metadataExpandSchemaParents;
    }

    public void setMetadataExpandSchemaParents(boolean metadataExpandSchemaParents) {
        this.metadataExpandSchemaParents = metadataExpandSchemaParents;
    }

    public String getMetadataFixtureCatalog() {
        return metadataFixtureCatalog;
    }

    public void setMetadataFixtureCatalog(String metadataFixtureCatalog) {
        this.metadataFixtureCatalog = normalizeOptionalText(metadataFixtureCatalog);
        if (this.metadataFixtureCatalog == null) {
            this.metadataFixtureCatalog = "";
        }
    }

    public String getRole() {
        return role;
    }

    public void setRole(String role) {
        this.role = role;
    }

    public String getApplicationName() {
        return applicationName;
    }

    public void setApplicationName(String applicationName) {
        this.applicationName = applicationName;
    }

    public boolean isReadOnly() {
        return readOnly;
    }

    public void setReadOnly(boolean readOnly) {
        this.readOnly = readOnly;
    }

    public boolean isAutoCommit() {
        return autoCommit;
    }

    public void setAutoCommit(boolean autoCommit) {
        this.autoCommit = autoCommit;
    }

    public int getDefaultRowFetchSize() {
        return defaultRowFetchSize;
    }

    public void setDefaultRowFetchSize(int defaultRowFetchSize) {
        this.defaultRowFetchSize = defaultRowFetchSize;
    }

    public int getPrepareThreshold() {
        return prepareThreshold;
    }

    public void setPrepareThreshold(int prepareThreshold) {
        this.prepareThreshold = prepareThreshold;
    }

    public boolean isBinaryTransfer() {
        return binaryTransfer;
    }

    public boolean isPooling() {
        return pooling;
    }

    public void setPooling(boolean pooling) {
        this.pooling = pooling;
    }

    public int getMinPoolSize() {
        return minPoolSize;
    }

    public void setMinPoolSize(int minPoolSize) {
        this.minPoolSize = Math.max(0, minPoolSize);
    }

    public int getMaxPoolSize() {
        return maxPoolSize;
    }

    public void setMaxPoolSize(int maxPoolSize) {
        this.maxPoolSize = Math.max(1, maxPoolSize);
    }

    public int getConnectionLifetime() {
        return connectionLifetime;
    }

    public void setConnectionLifetime(int connectionLifetime) {
        this.connectionLifetime = Math.max(0, connectionLifetime);
    }

    public int getAcquireTimeout() {
        return acquireTimeout;
    }

    public void setAcquireTimeout(int acquireTimeout) {
        this.acquireTimeout = Math.max(1, acquireTimeout);
    }

    public void setBinaryTransfer(boolean binaryTransfer) {
        this.binaryTransfer = binaryTransfer;
    }

    public String getCompression() {
        return compression;
    }

    public void setCompression(String compression) {
        this.compression = normalizeCompression(compression);
    }

    public String getManagerAuthToken() {
        return managerAuthToken;
    }

    public void setManagerAuthToken(String managerAuthToken) {
        this.managerAuthToken = managerAuthToken;
    }

    public String getManagerUsername() {
        return managerUsername;
    }

    public void setManagerUsername(String managerUsername) {
        this.managerUsername = managerUsername;
    }

    public String getManagerDatabase() {
        return managerDatabase;
    }

    public void setManagerDatabase(String managerDatabase) {
        this.managerDatabase = managerDatabase;
    }

    public String getManagerConnectionProfile() {
        return managerConnectionProfile;
    }

    public void setManagerConnectionProfile(String managerConnectionProfile) {
        this.managerConnectionProfile = managerConnectionProfile;
    }

    public String getManagerClientIntent() {
        return managerClientIntent;
    }

    public void setManagerClientIntent(String managerClientIntent) {
        this.managerClientIntent = managerClientIntent;
    }

    public int getManagerClientFlags() {
        return managerClientFlags;
    }

    public void setManagerClientFlags(int managerClientFlags) {
        this.managerClientFlags = managerClientFlags;
    }

    public boolean isManagerAuthFastPath() {
        return managerAuthFastPath;
    }

    public void setManagerAuthFastPath(boolean managerAuthFastPath) {
        this.managerAuthFastPath = managerAuthFastPath;
    }

    public int getConnectClientFlags() {
        return connectClientFlags;
    }

    public void setConnectClientFlags(int connectClientFlags) {
        this.connectClientFlags = connectClientFlags;
    }

    public String getAuthToken() {
        return authToken;
    }

    public void setAuthToken(String authToken) {
        this.authToken = authToken;
    }

    public String getAuthMethodId() {
        return authMethodId;
    }

    public void setAuthMethodId(String authMethodId) {
        if (authMethodId != null &&
            !authMethodId.trim().isEmpty() &&
            !authMethodId.startsWith("scratchbird.auth.")) {
            throw new IllegalArgumentException("invalid auth_method_id namespace");
        }
        this.authMethodId = authMethodId != null ? authMethodId.trim() : null;
    }

    public String getAuthMethodPayload() {
        return authMethodPayload;
    }

    public void setAuthMethodPayload(String authMethodPayload) {
        this.authMethodPayload = authMethodPayload;
    }

    public String getAuthPayloadJson() {
        return authPayloadJson;
    }

    public void setAuthPayloadJson(String authPayloadJson) {
        this.authPayloadJson = authPayloadJson;
    }

    public String getAuthPayloadB64() {
        return authPayloadB64;
    }

    public void setAuthPayloadB64(String authPayloadB64) {
        this.authPayloadB64 = authPayloadB64;
    }

    public String getAuthProviderProfile() {
        return authProviderProfile;
    }

    public void setAuthProviderProfile(String authProviderProfile) {
        this.authProviderProfile = authProviderProfile;
    }

    public String getAuthRequiredMethods() {
        return authRequiredMethods;
    }

    public void setAuthRequiredMethods(String authRequiredMethods) {
        this.authRequiredMethods = authRequiredMethods;
    }

    public String getAuthForbiddenMethods() {
        return authForbiddenMethods;
    }

    public void setAuthForbiddenMethods(String authForbiddenMethods) {
        this.authForbiddenMethods = authForbiddenMethods;
    }

    public boolean isAuthRequireChannelBinding() {
        return authRequireChannelBinding;
    }

    public void setAuthRequireChannelBinding(boolean authRequireChannelBinding) {
        this.authRequireChannelBinding = authRequireChannelBinding;
    }

    public String getWorkloadIdentityToken() {
        return workloadIdentityToken;
    }

    public void setWorkloadIdentityToken(String workloadIdentityToken) {
        this.workloadIdentityToken = workloadIdentityToken;
    }

    public String getProxyPrincipalAssertion() {
        return proxyPrincipalAssertion;
    }

    public void setProxyPrincipalAssertion(String proxyPrincipalAssertion) {
        this.proxyPrincipalAssertion = proxyPrincipalAssertion;
    }

    public boolean isReWriteBatchedInserts() {
        return reWriteBatchedInserts;
    }

    public void setReWriteBatchedInserts(boolean reWriteBatchedInserts) {
        this.reWriteBatchedInserts = reWriteBatchedInserts;
    }

    public String getLoggerLevel() {
        return loggerLevel;
    }

    public void setLoggerLevel(String loggerLevel) {
        this.loggerLevel = loggerLevel;
    }

    public String getLoggerFile() {
        return loggerFile;
    }

    public void setLoggerFile(String loggerFile) {
        this.loggerFile = loggerFile;
    }

    public Properties getExtraProperties() {
        return extraProperties;
    }

    /**
     * Checks if SSL is required.
     *
     * @return true if SSL mode requires encryption
     */
    public boolean isSslRequired() {
        String mode = getSslMode();
        return "require".equalsIgnoreCase(mode) ||
               "verify-ca".equalsIgnoreCase(mode) ||
               "verify-full".equalsIgnoreCase(mode);
    }

    /**
     * Checks if SSL certificate verification is required.
     *
     * @return true if SSL mode requires certificate verification
     */
    public boolean isSslVerify() {
        String mode = getSslMode();
        return "verify-ca".equalsIgnoreCase(mode) ||
               "verify-full".equalsIgnoreCase(mode);
    }

    /**
     * Converts to Properties object.
     *
     * @return Properties containing all settings
     */
    public Properties toProperties() {
        Properties props = new Properties();
        props.setProperty("host", host);
        props.setProperty("port", String.valueOf(port));
        props.setProperty("front_door_mode", frontDoorMode);
        props.setProperty("protocol", protocol);
        if (database != null) props.setProperty("database", database);
        if (user != null) props.setProperty("user", user);
        if (password != null) props.setProperty("password", password);
        props.setProperty("ssl", ssl);
        if (sslMode != null) props.setProperty("sslMode", sslMode);
        if (sslCert != null) props.setProperty("sslCert", sslCert);
        if (sslKey != null) props.setProperty("sslKey", sslKey);
        if (sslRootCert != null) props.setProperty("sslRootCert", sslRootCert);
        props.setProperty("connectTimeout", String.valueOf(connectTimeout));
        props.setProperty("socketTimeout", String.valueOf(socketTimeout));
        props.setProperty("loginTimeout", String.valueOf(loginTimeout));
        props.setProperty("tcpKeepAlive", String.valueOf(tcpKeepAlive));
        if (currentSchema != null) {
            props.setProperty("currentSchema", currentSchema);
        }
        props.setProperty("metadataExpandSchemaParents", String.valueOf(metadataExpandSchemaParents));
        if (metadataFixtureCatalog != null && !metadataFixtureCatalog.isEmpty()) {
            props.setProperty("metadata_fixture_catalog", metadataFixtureCatalog);
        }
        props.setProperty("pooling", String.valueOf(pooling));
        props.setProperty("maxPoolSize", String.valueOf(maxPoolSize));
        props.setProperty("minPoolSize", String.valueOf(minPoolSize));
        props.setProperty("connectionLifetime", String.valueOf(connectionLifetime));
        props.setProperty("acquireTimeout", String.valueOf(acquireTimeout));
        if (applicationName != null) props.setProperty("ApplicationName", applicationName);
        props.setProperty("readOnly", String.valueOf(readOnly));
        props.setProperty("autoCommit", String.valueOf(autoCommit));
        props.setProperty("defaultRowFetchSize", String.valueOf(defaultRowFetchSize));
        props.setProperty("prepareThreshold", String.valueOf(prepareThreshold));
        props.setProperty("binaryTransfer", String.valueOf(binaryTransfer));
        props.setProperty("compression", compression);
        if (managerAuthToken != null && !managerAuthToken.isEmpty()) props.setProperty("manager_auth_token", managerAuthToken);
        if (managerUsername != null && !managerUsername.isEmpty()) props.setProperty("manager_username", managerUsername);
        if (managerDatabase != null && !managerDatabase.isEmpty()) props.setProperty("manager_database", managerDatabase);
        if (managerConnectionProfile != null && !managerConnectionProfile.isEmpty()) props.setProperty("manager_connection_profile", managerConnectionProfile);
        if (managerClientIntent != null && !managerClientIntent.isEmpty()) props.setProperty("manager_client_intent", managerClientIntent);
        props.setProperty("manager_client_flags", String.valueOf(managerClientFlags));
        props.setProperty("manager_auth_fast_path", String.valueOf(managerAuthFastPath));
        props.setProperty("connect_client_flags", String.valueOf(connectClientFlags));
        if (authToken != null && !authToken.isEmpty()) props.setProperty("auth_token", authToken);
        if (authMethodId != null && !authMethodId.isEmpty()) props.setProperty("auth_method_id", authMethodId);
        if (authMethodPayload != null && !authMethodPayload.isEmpty()) props.setProperty("auth_method_payload", authMethodPayload);
        if (authPayloadJson != null && !authPayloadJson.isEmpty()) props.setProperty("auth_payload_json", authPayloadJson);
        if (authPayloadB64 != null && !authPayloadB64.isEmpty()) props.setProperty("auth_payload_b64", authPayloadB64);
        if (authProviderProfile != null && !authProviderProfile.isEmpty()) props.setProperty("auth_provider_profile", authProviderProfile);
        if (authRequiredMethods != null && !authRequiredMethods.isEmpty()) props.setProperty("auth_required_methods", authRequiredMethods);
        if (authForbiddenMethods != null && !authForbiddenMethods.isEmpty()) props.setProperty("auth_forbidden_methods", authForbiddenMethods);
        props.setProperty("auth_require_channel_binding", String.valueOf(authRequireChannelBinding));
        if (workloadIdentityToken != null && !workloadIdentityToken.isEmpty()) props.setProperty("workload_identity_token", workloadIdentityToken);
        if (proxyPrincipalAssertion != null && !proxyPrincipalAssertion.isEmpty()) props.setProperty("proxy_principal_assertion", proxyPrincipalAssertion);
        props.setProperty("reWriteBatchedInserts", String.valueOf(reWriteBatchedInserts));
        props.setProperty("loggerLevel", loggerLevel);
        if (loggerFile != null) props.setProperty("loggerFile", loggerFile);
        props.putAll(extraProperties);
        return props;
    }

    @Override
    public String toString() {
        return "SBConnectionProperties{" +
               "host='" + host + '\'' +
               ", port=" + port +
               ", protocol='" + protocol + '\'' +
               ", database='" + database + '\'' +
               ", user='" + user + '\'' +
               ", ssl='" + ssl + '\'' +
               ", currentSchema='" + currentSchema + '\'' +
               '}';
    }

    private static String normalizeNativeProtocol(String value) {
        String normalized = value == null ? "" : value.trim().toLowerCase();
        switch (normalized) {
            case "":
            case "native":
            case "scratchbird":
            case "scratchbird-native":
            case "scratchbird_native":
                return "native";
            default:
                throw new IllegalArgumentException(
                    "Only protocol=native is supported; connect to the native parser listener/port.");
        }
    }

    private static String normalizeFrontDoorMode(String value) {
        String normalized = value == null ? "" : value.trim().toLowerCase();
        switch (normalized) {
            case "":
            case "direct":
                return "direct";
            case "manager_proxy":
            case "manager-proxy":
            case "managed":
                return "manager_proxy";
            default:
                throw new IllegalArgumentException("front_door_mode must be direct or manager_proxy.");
        }
    }

    private static String normalizeCompression(String value) {
        String normalized = value == null ? "" : value.trim().toLowerCase();
        switch (normalized) {
            case "":
            case "off":
            case "none":
                return "off";
            case "zstd":
                return "zstd";
            default:
                throw new IllegalArgumentException("compression must be off or zstd.");
        }
    }

    private static String normalizeOptionalText(String value) {
        if (value == null) {
            return null;
        }
        String trimmed = value.trim();
        return trimmed.isEmpty() ? null : trimmed;
    }

    private static int parseOptionalInt(String value, int currentValue) {
        String normalized = normalizeOptionalText(value);
        return normalized == null ? currentValue : Integer.parseInt(normalized);
    }

    private static boolean parseOptionalBoolean(String value, boolean currentValue) {
        String normalized = normalizeOptionalText(value);
        return normalized == null ? currentValue : Boolean.parseBoolean(normalized);
    }
}
