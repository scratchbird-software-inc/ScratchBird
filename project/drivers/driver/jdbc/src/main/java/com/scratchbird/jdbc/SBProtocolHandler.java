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

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.sql.SQLDataException;
import java.sql.SQLFeatureNotSupportedException;
import java.sql.SQLIntegrityConstraintViolationException;
import java.sql.SQLInvalidAuthorizationSpecException;
import java.sql.SQLNonTransientConnectionException;
import java.sql.SQLNonTransientException;
import java.sql.SQLException;
import java.sql.SQLSyntaxErrorException;
import java.sql.SQLTimeoutException;
import java.sql.SQLTransactionRollbackException;
import java.sql.SQLTransientConnectionException;
import java.sql.SQLTransientException;
import java.sql.SQLWarning;
import java.security.KeyStore;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Base64;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Locale;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;
import javax.net.ssl.KeyManager;
import javax.net.ssl.KeyManagerFactory;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;
import javax.net.ssl.TrustManager;
import javax.net.ssl.TrustManagerFactory;

/**
 * Protocol handler for ScratchBird native wire protocol.
 */
public class SBProtocolHandler {

    public interface NotificationListener {
        void onNotification(NotificationMessage notification);
    }

    public static final class NotificationMessage {
        public final int processId;
        public final String channel;
        public final byte[] payload;
        public final Character changeType;
        public final Long rowId;

        NotificationMessage(int processId, String channel, byte[] payload, Character changeType, Long rowId) {
            this.processId = processId;
            this.channel = channel;
            this.payload = payload;
            this.changeType = changeType;
            this.rowId = rowId;
        }
    }

    public static final class QueryPlanMessage {
        public final int format;
        public final long planningTimeUs;
        public final long estimatedRows;
        public final long estimatedCost;
        public final byte[] plan;

        QueryPlanMessage(int format, long planningTimeUs, long estimatedRows, long estimatedCost, byte[] plan) {
            this.format = format;
            this.planningTimeUs = planningTimeUs;
            this.estimatedRows = estimatedRows;
            this.estimatedCost = estimatedCost;
            this.plan = plan;
        }
    }

    public static final class SblrCompiledMessage {
        public final long hash;
        public final int version;
        public final byte[] bytecode;

        SblrCompiledMessage(long hash, int version, byte[] bytecode) {
            this.hash = hash;
            this.version = version;
            this.bytecode = bytecode;
        }
    }

    private static final int PROTOCOL_MAGIC = 0x50574253;
    private static final int PROTOCOL_VERSION_MAJOR = 1;
    private static final int PROTOCOL_VERSION_MINOR = 1;
    private static final int HEADER_SIZE = 40;
    private static final int MAX_MESSAGE_SIZE = 1024 * 1024 * 1024;
    private static final int MANAGER_PROTOCOL_MAGIC = 0x42444253;
    private static final int MANAGER_PROTOCOL_VERSION = 0x0101;
    private static final int MANAGER_HEADER_SIZE = 12;
    private static final int MANAGER_MAX_PAYLOAD_SIZE = 16 * 1024 * 1024;
    private static final int MCP_PROTOCOL_VERSION = 0x0100;

    private static final byte MSG_STARTUP = 0x01;
    private static final byte MSG_AUTH_RESPONSE = 0x02;
    private static final byte MSG_QUERY = 0x03;
    private static final byte MSG_PARSE = 0x04;
    private static final byte MSG_BIND = 0x05;
    private static final byte MSG_DESCRIBE = 0x06;
    private static final byte MSG_EXECUTE = 0x07;
    private static final byte MSG_CLOSE = 0x08;
    private static final byte MSG_SYNC = 0x09;
    private static final byte MSG_FLUSH = 0x0A;
    private static final byte MSG_CANCEL = 0x0B;
    private static final byte MSG_TERMINATE = 0x0C;
    private static final byte MSG_COPY_DATA = 0x0D;
    private static final byte MSG_COPY_DONE = 0x0E;
    private static final byte MSG_COPY_FAIL = 0x0F;
    private static final byte MSG_SBLR_EXECUTE = 0x10;
    private static final byte MSG_SUBSCRIBE = 0x11;
    private static final byte MSG_UNSUBSCRIBE = 0x12;
    private static final byte MSG_FEDERATED_QUERY = 0x13;
    private static final byte MSG_STREAM_CONTROL = 0x14;
    private static final byte MSG_TXN_BEGIN = 0x15;
    private static final byte MSG_TXN_COMMIT = 0x16;
    private static final byte MSG_TXN_ROLLBACK = 0x17;
    private static final byte MSG_TXN_SAVEPOINT = 0x18;
    private static final byte MSG_TXN_RELEASE = 0x19;
    private static final byte MSG_TXN_ROLLBACK_TO = 0x1A;
    private static final byte MSG_PING = 0x1B;
    private static final byte MSG_SET_OPTION = 0x1C;
    private static final byte MSG_CLUSTER_AUTH = 0x1D;
    private static final byte MSG_ATTACH_CREATE = 0x1E;
    private static final byte MSG_ATTACH_DETACH = 0x1F;
    private static final byte MSG_ATTACH_LIST = 0x20;

    private static final byte MSG_AUTH_REQUEST = 0x40;
    private static final byte MSG_AUTH_OK = 0x41;
    private static final byte MSG_AUTH_CONTINUE = 0x42;
    private static final byte MSG_READY = 0x43;
    private static final byte MSG_ROW_DESCRIPTION = 0x44;
    private static final byte MSG_DATA_ROW = 0x45;
    private static final byte MSG_COMMAND_COMPLETE = 0x46;
    private static final byte MSG_EMPTY_QUERY = 0x47;
    private static final byte MSG_ERROR = 0x48;
    private static final byte MSG_NOTICE = 0x49;
    private static final byte MSG_PARSE_COMPLETE = 0x4A;
    private static final byte MSG_BIND_COMPLETE = 0x4B;
    private static final byte MSG_CLOSE_COMPLETE = 0x4C;
    private static final byte DESCRIBE_STATEMENT = 'S';
    private static final byte DESCRIBE_PORTAL = 'P';
    private static final byte MSG_PORTAL_SUSPENDED = 0x4D;
    private static final byte MSG_NO_DATA = 0x4E;
    private static final byte MSG_PARAMETER_STATUS = 0x4F;
    private static final byte MSG_PARAMETER_DESCRIPTION = 0x50;
    private static final byte MSG_COPY_IN_RESPONSE = 0x51;
    private static final byte MSG_COPY_OUT_RESPONSE = 0x52;
    private static final byte MSG_COPY_BOTH_RESPONSE = 0x53;
    private static final byte MSG_NOTIFICATION = 0x54;
    private static final byte MSG_FUNCTION_RESULT = 0x55;
    private static final byte MSG_NEGOTIATE_VERSION = 0x56;
    private static final byte MSG_SBLR_COMPILED = 0x57;
    private static final byte MSG_QUERY_PLAN = 0x58;
    private static final byte MSG_STREAM_READY = 0x59;
    private static final byte MSG_STREAM_DATA = 0x5A;
    private static final byte MSG_STREAM_END = 0x5B;
    private static final byte MSG_TXN_STATUS = 0x5C;
    private static final byte MSG_PONG = 0x5D;
    private static final byte MSG_CLUSTER_AUTH_OK = 0x5E;
    private static final byte MSG_FEDERATED_RESULT = 0x5F;
    private static final byte MSG_HEARTBEAT = (byte) 0x80;
    private static final byte MSG_EXTENSION = (byte) 0x81;

    private static final int AUTH_OK = 0;
    private static final int AUTH_PASSWORD = 1;
    private static final int AUTH_MD5 = 2;
    private static final int AUTH_SCRAM_SHA_256 = 3;
    private static final int AUTH_SCRAM_SHA_512 = 4;
    private static final int AUTH_TOKEN = 5;
    private static final int AUTH_PEER = 6;
    private static final int AUTH_REATTACH = 7;
    private static final byte MCP_MSG_CONNECT_RESPONSE = 0x02;
    private static final byte MCP_MSG_AUTH_CHALLENGE = 0x12;
    private static final byte MCP_MSG_AUTH_RESPONSE = 0x11;
    private static final byte MCP_MSG_STATUS_RESPONSE = 0x64;
    private static final byte MCP_MSG_HELLO = 0x65;
    private static final byte MCP_MSG_AUTH_START = 0x66;
    private static final byte MCP_MSG_AUTH_CONTINUE = 0x67;
    private static final byte MCP_MSG_DB_CONNECT = 0x69;
    private static final byte MCP_AUTH_METHOD_TOKEN = 4;

    private static final byte MSG_FLAG_URGENT = 0x08;

    private static final long FEATURE_COMPRESSION = 1L << 0;
    private static final long FEATURE_STREAMING = 1L << 1;

    private static final int QUERY_FLAG_DESCRIBE_ONLY = 0x01;
    private static final int QUERY_FLAG_NO_PORTAL = 0x02;
    private static final int QUERY_FLAG_BINARY_RESULT = 0x04;
    private static final int QUERY_FLAG_INCLUDE_PLAN = 0x08;
    private static final int QUERY_FLAG_RETURN_SBLR = 0x10;
    private static final int QUERY_FLAG_NO_CACHE = 0x20;
    private static final int MAX_PREPARED_STATEMENTS = 256;

    static final byte ISOLATION_READ_UNCOMMITTED = 0;
    static final byte ISOLATION_READ_COMMITTED = 1;
    static final byte ISOLATION_REPEATABLE_READ = 2;
    static final byte ISOLATION_SERIALIZABLE = 3;

    public static final byte READ_COMMITTED_MODE_DEFAULT = 0;
    public static final byte READ_COMMITTED_MODE_READ_CONSISTENCY = 1;
    public static final byte READ_COMMITTED_MODE_RECORD_VERSION = 2;
    public static final byte READ_COMMITTED_MODE_NO_RECORD_VERSION = 3;

    private static final short TXN_FLAG_HAS_ISOLATION = 0x0001;
    private static final short TXN_FLAG_HAS_ACCESS = 0x0002;
    private static final short TXN_FLAG_HAS_DEFERRABLE = 0x0004;
    private static final short TXN_FLAG_HAS_WAIT = 0x0008;
    private static final short TXN_FLAG_HAS_TIMEOUT = 0x0010;
    private static final short TXN_FLAG_HAS_AUTOCOMMIT = 0x0020;
    private static final short TXN_FLAG_HAS_READ_COMMITTED_MODE = 0x0100;

    private static final ScheduledExecutorService TIMEOUT_SCHEDULER =
        Executors.newSingleThreadScheduledExecutor(new ThreadFactory() {
            @Override
            public Thread newThread(Runnable r) {
                Thread t = new Thread(r, "sbjdbc-timeouts");
                t.setDaemon(true);
                return t;
            }
        });

    private final SBConnectionProperties props;

    private Socket socket;
    private InputStream inputStream;
    private OutputStream outputStream;

    private boolean connected = false;
    private int networkTimeout = 0;
    private int sequence = 0;
    private byte[] attachmentId = new byte[16];
    private long txnId = 0;
    private boolean runtimeTxnActive = false;

    private final Map<String, String> serverParameters = new HashMap<>();
    private int lastQuerySequence = 0;
    private QueryPlanMessage lastPlan;
    private SblrCompiledMessage lastSblr;
    private final CopyOnWriteArrayList<NotificationListener> notificationListeners = new CopyOnWriteArrayList<>();
    private final Map<String, PreparedStatementCacheEntry> preparedStatements =
        new LinkedHashMap<>(16, 0.75f, true);
    private int preparedStatementSequence = 0;
    private boolean portalResumePending = false;
    private SBResolvedAuthContext resolvedAuthContext = new SBResolvedAuthContext();

    public SBProtocolHandler(SBConnectionProperties props) {
        this.props = props;
        resetResolvedAuthContext();
    }

    public synchronized SBResolvedAuthContext getResolvedAuthContext() {
        return resolvedAuthContext.copy();
    }

    private void resetResolvedAuthContext() {
        resolvedAuthContext = new SBResolvedAuthContext();
        resolvedAuthContext.setFrontDoorMode(
            props.getFrontDoorMode() == null || props.getFrontDoorMode().isBlank()
                ? "direct"
                : props.getFrontDoorMode());
    }

    private void markResolvedAuthContextDetached() {
        resolvedAuthContext.setAttached(false);
        resolvedAuthContext.setManagerAuthenticated(false);
    }

    public void addNotificationListener(NotificationListener listener) {
        if (listener != null && !notificationListeners.contains(listener)) {
            notificationListeners.add(listener);
        }
    }

    public void removeNotificationListener(NotificationListener listener) {
        if (listener != null) {
            notificationListeners.remove(listener);
        }
    }

    public QueryPlanMessage getLastQueryPlan() {
        return lastPlan;
    }

    public SblrCompiledMessage getLastSblrCompiled() {
        return lastSblr;
    }

    public synchronized void connect() throws SQLException {
        try {
            resetResolvedAuthContext();
            openTransport();
            if ("manager_proxy".equalsIgnoreCase(props.getFrontDoorMode())) {
                performManagerConnect();
            }
            sendStartupMessage();
            handleAuthentication();
            connected = true;

        } catch (SQLException e) {
            close();
            throw e;
        } catch (IOException e) {
            close();
            throw createSQLException("Failed to connect: " + e.getMessage(), "08001", e);
        }
    }

    public synchronized SBAuthProbeResult probeAuthSurface() throws SQLException {
        try {
            resetResolvedAuthContext();
            openTransport();
            String resolvedHost = (props.getHost() == null || props.getHost().isBlank())
                ? "localhost"
                : props.getHost();
            int resolvedPort = props.getPort() > 0 ? props.getPort() : SBDriver.DEFAULT_PORT;
            if ("manager_proxy".equalsIgnoreCase(props.getFrontDoorMode())) {
                return probeManagerAuthSurface(resolvedHost, resolvedPort);
            }
            return probeDirectAuthSurface(resolvedHost, resolvedPort);
        } catch (IOException e) {
            close();
            throw createSQLException("Failed to probe auth surface: " + e.getMessage(), "08001", e);
        } finally {
            close();
        }
    }

    private void openTransport() throws IOException, SQLException {
        props.setProtocol(props.getProtocol());
        props.setFrontDoorMode(props.getFrontDoorMode());
        String host = props.getHost();
        if (host == null || host.isBlank()) {
            host = "localhost";
            props.setHost(host);
        }
        if (props.getPort() <= 0) {
            props.setPort(SBDriver.DEFAULT_PORT);
        }

        socket = new Socket();
        socket.setTcpNoDelay(true);
        socket.setKeepAlive(props.isTcpKeepAlive());
        if (props.getSocketTimeout() > 0) {
            socket.setSoTimeout(props.getSocketTimeout() * 1000);
        }

        InetSocketAddress address = new InetSocketAddress(host, props.getPort());
        socket.connect(address, props.getConnectTimeout() * 1000);
        inputStream = new BufferedInputStream(socket.getInputStream(), 65536);
        outputStream = new BufferedOutputStream(socket.getOutputStream(), 65536);

        String sslMode = props.getSslMode();
        if (sslMode == null || sslMode.isEmpty()) {
            sslMode = "require";
        }
        if (!"disable".equalsIgnoreCase(sslMode)) {
            upgradeToSSL(sslMode);
        }
    }

    public synchronized SBQueryResult execute(String sql) throws SQLException {
        return execute(sql, Collections.emptyList(), Collections.emptyList(), 0, 0);
    }

    public synchronized SBQueryResult execute(String sql, int maxRows, int timeoutMs) throws SQLException {
        return execute(sql, Collections.emptyList(), Collections.emptyList(), maxRows, timeoutMs);
    }

    public synchronized SBQueryResult executeNoCache(String sql, int maxRows, int timeoutMs) throws SQLException {
        ScheduledFuture<?> cancelTask = scheduleCancel(timeoutMs);
        try {
            if (isSchemaMutation(sql)) {
                clearPreparedStatements();
            }
            sendSimpleQuery(sql, maxRows, timeoutMs, QUERY_FLAG_NO_CACHE);
            return readQueryResult();
        } catch (IOException e) {
            throw createSQLException("Query execution failed: " + e.getMessage(), "08006", e);
        } finally {
            if (cancelTask != null) {
                cancelTask.cancel(false);
            }
        }
    }

    public synchronized SBQueryResult execute(String sql, List<Object> params, List<Integer> paramTypes,
                                 int maxRows, int timeoutMs) throws SQLException {
        ScheduledFuture<?> cancelTask = scheduleCancel(timeoutMs);
        try {
            if (params == null || params.isEmpty()) {
                if (isSchemaMutation(sql)) {
                    clearPreparedStatements();
                }
                sendSimpleQuery(sql, maxRows, timeoutMs, 0);
                return readQueryResult();
            }

            int attempts = 0;
            while (true) {
                try {
                    sendExtendedQuery(sql, params, paramTypes, maxRows);
                    return readQueryResult();
                } catch (SQLException ex) {
                    if (attempts++ == 0 && isRecoverableCachedStatementError(ex)) {
                        clearPreparedStatements();
                        continue;
                    }
                    throw ex;
                }
            }
        } catch (IOException e) {
            throw createSQLException("Query execution failed: " + e.getMessage(), "08006", e);
        } finally {
            if (cancelTask != null) {
                cancelTask.cancel(false);
            }
        }
    }

    public synchronized SBQueryResult executeStreaming(String sql, int pageSize, int timeoutMs) throws SQLException {
        return executeStreaming(sql, Collections.emptyList(), Collections.emptyList(), pageSize, timeoutMs);
    }

    public synchronized SBQueryResult executeStreaming(String sql, List<Object> params, List<Integer> paramTypes,
                                          int pageSize, int timeoutMs) throws SQLException {
        ScheduledFuture<?> cancelTask = scheduleCancel(timeoutMs);
        try {
            if (params == null || params.isEmpty()) {
                sendSimpleQuery(sql, pageSize, timeoutMs, 0);
            } else {
                sendExtendedQuery(sql, params, paramTypes, pageSize);
            }
            SBQueryResult result = new SBQueryResult();
            result.setStream(new StreamingCursor(this, pageSize, cancelTask));
            return result;
        } catch (IOException e) {
            if (cancelTask != null) {
                cancelTask.cancel(false);
            }
            throw createSQLException("Query execution failed: " + e.getMessage(), "08006", e);
        }
    }

    public synchronized void beginTransaction() throws SQLException {
        beginTransaction(
            ISOLATION_READ_COMMITTED,
            (byte) 0,
            false,
            false,
            0,
            (byte) 0,
            (byte) 0,
            READ_COMMITTED_MODE_DEFAULT
        );
    }

    public static SBRetryScope retryScopeForSqlState(String state) {
        // Drivers are fail-closed: fresh statement restart for 40xxx,
        // reconnect only for 08xxx, and no automatic whole-transaction replay.
        if (state == null || state.length() != 5) {
            return SBRetryScope.NONE;
        }
        if ("40001".equals(state) || "40P01".equals(state)) {
            return SBRetryScope.STATEMENT;
        }
        if (state.startsWith("08")) {
            return SBRetryScope.RECONNECT;
        }
        return SBRetryScope.NONE;
    }

    public static boolean isRetryableSqlState(String state) {
        return retryScopeForSqlState(state) != SBRetryScope.NONE;
    }

    public synchronized void beginTransaction(byte isolationLevel, byte accessMode, boolean deferrable,
                                 boolean wait, int timeoutMs, byte autocommitMode, byte conflictAction) throws SQLException {
        beginTransaction(
            isolationLevel,
            accessMode,
            deferrable,
            wait,
            timeoutMs,
            autocommitMode,
            conflictAction,
            READ_COMMITTED_MODE_DEFAULT
        );
    }

    public synchronized void beginTransaction(byte isolationLevel, byte accessMode, boolean deferrable,
                                 boolean wait, int timeoutMs, byte autocommitMode, byte conflictAction,
                                 byte readCommittedMode) throws SQLException {
        try {
            short flags = TXN_FLAG_HAS_ISOLATION;
            if (accessMode != 0) flags |= TXN_FLAG_HAS_ACCESS;
            if (deferrable) flags |= TXN_FLAG_HAS_DEFERRABLE;
            if (wait) flags |= TXN_FLAG_HAS_WAIT;
            if (timeoutMs > 0) flags |= TXN_FLAG_HAS_TIMEOUT;
            if (autocommitMode != 0) flags |= TXN_FLAG_HAS_AUTOCOMMIT;
            if (readCommittedMode != READ_COMMITTED_MODE_DEFAULT) flags |= TXN_FLAG_HAS_READ_COMMITTED_MODE;
            byte[] payload = buildTxnBeginPayload(flags, conflictAction, autocommitMode, isolationLevel,
                accessMode, (byte) (deferrable ? 1 : 0), (byte) (wait ? 1 : 0), timeoutMs, readCommittedMode);
            sendMessage(MSG_TXN_BEGIN, payload, (byte) 0, false);
            drainUntilReady();
        } catch (IOException e) {
            throw createSQLException("Failed to begin transaction: " + e.getMessage(), "08006", e);
        }
    }

    public static String canonicalReadCommittedModeLabel(int mode) {
        switch (mode) {
            case READ_COMMITTED_MODE_DEFAULT:
                return "READ COMMITTED";
            case READ_COMMITTED_MODE_READ_CONSISTENCY:
                return "READ COMMITTED READ CONSISTENCY";
            case READ_COMMITTED_MODE_RECORD_VERSION:
                return "READ COMMITTED RECORD VERSION";
            case READ_COMMITTED_MODE_NO_RECORD_VERSION:
                return "READ COMMITTED NO RECORD VERSION";
            default:
                return "UNKNOWN(" + mode + ")";
        }
    }

    public synchronized void commitTransaction(byte flags) throws SQLException {
        try {
            sendMessage(MSG_TXN_COMMIT, buildTxnCommitPayload(flags), (byte) 0, false);
            drainUntilReady();
        } catch (IOException e) {
            throw createSQLException("Failed to commit transaction: " + e.getMessage(), "08006", e);
        }
    }

    public synchronized void rollbackTransaction(byte flags) throws SQLException {
        try {
            sendMessage(MSG_TXN_ROLLBACK, buildTxnRollbackPayload(flags), (byte) 0, false);
            drainUntilReady();
        } catch (IOException e) {
            throw createSQLException("Failed to rollback transaction: " + e.getMessage(), "08006", e);
        }
    }

    public synchronized void savepoint(String name) throws SQLException {
        try {
            sendMessage(MSG_TXN_SAVEPOINT, buildTxnSavepointPayload(name), (byte) 0, false);
            drainUntilReady();
        } catch (IOException e) {
            throw createSQLException("Failed to create savepoint: " + e.getMessage(), "08006", e);
        }
    }

    public synchronized void releaseSavepoint(String name) throws SQLException {
        try {
            sendMessage(MSG_TXN_RELEASE, buildTxnReleasePayload(name), (byte) 0, false);
            drainUntilReady();
        } catch (IOException e) {
            throw createSQLException("Failed to release savepoint: " + e.getMessage(), "08006", e);
        }
    }

    public synchronized void rollbackToSavepoint(String name) throws SQLException {
        try {
            sendMessage(MSG_TXN_ROLLBACK_TO, buildTxnRollbackToPayload(name), (byte) 0, false);
            drainUntilReady();
        } catch (IOException e) {
            throw createSQLException("Failed to rollback savepoint: " + e.getMessage(), "08006", e);
        }
    }

    public synchronized void setOption(String name, String value) throws SQLException {
        try {
            sendMessage(MSG_SET_OPTION, buildSetOptionPayload(name, value), (byte) 0, false);
            drainUntilReady();
        } catch (IOException e) {
            throw createSQLException("Failed to set option: " + e.getMessage(), "08006", e);
        }
    }

    public synchronized void ping() throws SQLException {
        try {
            sendMessage(MSG_PING, new byte[0], (byte) 0, false);
            while (true) {
                ProtocolMessage msg = readMessage();
                if (handleAsyncMessage(msg)) {
                    continue;
                }
                if (msg.type == MSG_PONG) {
                    return;
                }
                if (msg.type == MSG_READY) {
                    ReadyStatus ready = parseReady(msg.payload);
                    applyRuntimeReadyState(ready);
                    return;
                }
                if (msg.type == MSG_ERROR) {
                    ProtocolError error = parseErrorMessage(msg.payload);
                    throw createSQLException(buildErrorMessage(error),
                        error.sqlState != null ? error.sqlState : "42000");
                }
            }
        } catch (IOException e) {
            throw createSQLException("Failed to ping server: " + e.getMessage(), "08006", e);
        }
    }

    public synchronized void subscribe(byte subscribeType, String channel, String filterExpr) throws SQLException {
        try {
            sendMessage(MSG_SUBSCRIBE, buildSubscribePayload(subscribeType, channel, filterExpr), (byte) 0, false);
            drainUntilReady();
        } catch (IOException e) {
            throw createSQLException("Failed to subscribe: " + e.getMessage(), "08006", e);
        }
    }

    public synchronized void unsubscribe(String channel) throws SQLException {
        try {
            sendMessage(MSG_UNSUBSCRIBE, buildUnsubscribePayload(channel), (byte) 0, false);
            drainUntilReady();
        } catch (IOException e) {
            throw createSQLException("Failed to unsubscribe: " + e.getMessage(), "08006", e);
        }
    }

    public synchronized SBQueryResult executeSblr(long sblrHash, byte[] bytecode, List<Object> params, List<Integer> paramTypes)
        throws SQLException {
        try {
            List<SBTypeCodec.ParamEncoding> encoded = new ArrayList<>();
            for (int i = 0; i < params.size(); i++) {
                Integer sqlType = (paramTypes != null && i < paramTypes.size()) ? paramTypes.get(i) : null;
                encoded.add(SBTypeCodec.encodeParam(params.get(i), sqlType));
            }
            lastPlan = null;
            lastSblr = null;
            lastQuerySequence = sequence;
            sendMessage(MSG_SBLR_EXECUTE, buildSblrExecutePayload(sblrHash, bytecode, encoded), (byte) 0, false);
            sendMessage(MSG_SYNC, new byte[0], (byte) 0, false);
            return readQueryResult();
        } catch (IOException e) {
            throw createSQLException("SBLR execution failed: " + e.getMessage(), "08006", e);
        }
    }

    public synchronized void streamControl(byte controlType, int windowSize, int timeoutMs) throws SQLException {
        try {
            sendMessage(MSG_STREAM_CONTROL, buildStreamControlPayload(controlType, windowSize, timeoutMs), (byte) 0, false);
        } catch (IOException e) {
            throw createSQLException("Failed to send stream control: " + e.getMessage(), "08006", e);
        }
    }

    public synchronized void attachCreate(String emulationMode, String dbName) throws SQLException {
        try {
            sendMessage(MSG_ATTACH_CREATE, buildAttachCreatePayload(emulationMode, dbName), (byte) 0, false);
            drainUntilReady();
        } catch (IOException e) {
            throw createSQLException("Failed to create attachment: " + e.getMessage(), "08006", e);
        }
    }

    public synchronized void attachDetach() throws SQLException {
        try {
            sendMessage(MSG_ATTACH_DETACH, new byte[0], (byte) 0, false);
            drainUntilReady();
        } catch (IOException e) {
            throw createSQLException("Failed to detach attachment: " + e.getMessage(), "08006", e);
        }
    }

    public synchronized SBQueryResult attachList() throws SQLException {
        try {
            sendMessage(MSG_ATTACH_LIST, new byte[0], (byte) 0, false);
            sendMessage(MSG_SYNC, new byte[0], (byte) 0, false);
            return readQueryResult();
        } catch (IOException e) {
            throw createSQLException("Failed to list attachments: " + e.getMessage(), "08006", e);
        }
    }

    public synchronized void cancelCurrentQuery() throws SQLException {
        if (!connected) return;
        try {
            sendMessage(MSG_CANCEL, buildCancelPayload(0, lastQuerySequence), MSG_FLAG_URGENT, false);
        } catch (IOException e) {
            throw createSQLException("Failed to cancel query: " + e.getMessage(), "08006", e);
        }
    }

    private ScheduledFuture<?> scheduleCancel(int timeoutMs) {
        if (timeoutMs <= 0) {
            return null;
        }
        return TIMEOUT_SCHEDULER.schedule(() -> {
            try {
                cancelCurrentQuery();
            } catch (SQLException e) {
                // Ignore cancellation errors.
            }
        }, timeoutMs, TimeUnit.MILLISECONDS);
    }

    public synchronized boolean isAlive(int timeout) {
        if (!connected || socket == null || socket.isClosed()) {
            return false;
        }
        try {
            int oldTimeout = socket.getSoTimeout();
            socket.setSoTimeout(timeout * 1000);
            try {
                sendMessage(MSG_SYNC, new byte[0], (byte) 0, false);
                while (true) {
                    ProtocolMessage msg = readMessage();
                    try {
                        if (handleAsyncMessage(msg)) {
                            continue;
                        }
                    } catch (SQLException e) {
                        return false;
                    }
                    if (msg.type == MSG_READY) {
                        ReadyStatus ready = parseReady(msg.payload);
                        applyRuntimeReadyState(ready);
                        return true;
                    }
                    if (msg.type == MSG_ERROR) {
                        return false;
                    }
                }
            } finally {
                socket.setSoTimeout(oldTimeout);
            }
        } catch (IOException e) {
            return false;
        }
    }

    public synchronized void abort() {
        try {
            if (socket != null) {
                socket.close();
            }
        } catch (IOException e) {
            // Ignore
        }
        clearTransactionState();
        portalResumePending = false;
        connected = false;
        markResolvedAuthContextDetached();
    }

    public synchronized void close() {
        try {
            if (socket != null) {
                socket.close();
            }
        } catch (IOException e) {
            // Ignore
        }
        clearTransactionState();
        portalResumePending = false;
        connected = false;
        markResolvedAuthContextDetached();
    }

    synchronized void allowPortalResume() {
        portalResumePending = true;
    }

    synchronized void resumeSuspendedPortal(int pageSize) throws SQLException {
        if (!portalResumePending) {
            throw createSQLException("portal resume requires explicit suspended state", "55000");
        }
        portalResumePending = false;
        int nextPageSize = Math.max(pageSize, 1);
        byte[] payload = buildExecutePayload("", nextPageSize);
        try {
            sendMessage(MSG_EXECUTE, payload, (byte) 0, false);
        } catch (IOException e) {
            throw createSQLException("Failed to resume portal: " + e.getMessage(), "08006", e);
        }
    }

    synchronized void closeStreamingPortal() throws SQLException {
        try {
            portalResumePending = false;
            sendMessage(MSG_CLOSE, buildClosePayload(DESCRIBE_PORTAL, ""), (byte) 0, false);
            sendMessage(MSG_SYNC, new byte[0], (byte) 0, false);
        } catch (IOException e) {
            throw createSQLException("Failed to close streaming portal: " + e.getMessage(), "08006", e);
        }
    }

    public synchronized boolean isConnected() {
        return connected && socket != null && !socket.isClosed() && inputStream != null && outputStream != null;
    }

    public synchronized void setNetworkTimeout(int milliseconds) {
        this.networkTimeout = milliseconds;
        try {
            if (socket != null) {
                socket.setSoTimeout(milliseconds);
            }
        } catch (IOException e) {
            // Ignore
        }
    }

    public int getNetworkTimeout() {
        return networkTimeout;
    }

    public synchronized boolean hasActiveTransaction() {
        return runtimeTxnActive || txnId != 0;
    }

    private void applyRuntimeTxnId(long runtimeTxnId) {
        txnId = runtimeTxnId;
        if (runtimeTxnId > 0) {
            runtimeTxnActive = true;
        }
    }

    private void applyRuntimeReadyState(ReadyStatus ready) {
        txnId = ready.txnId;
        if (ready.status != 0) {
            // READY is authoritative for native MGA activity. The engine can
            // publish a fresh active boundary while the public wire header
            // still reports txn_id == 0.
            runtimeTxnActive = true;
            return;
        }
        clearTransactionState();
    }

    private void clearTransactionState() {
        txnId = 0;
        runtimeTxnActive = false;
    }

    public String getServerParameter(String name) {
        return serverParameters.get(name);
    }

    private byte[] buildLengthPrefixedString(String value) {
        byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
        ByteBuffer buf = ByteBuffer.allocate(4 + bytes.length).order(ByteOrder.LITTLE_ENDIAN);
        buf.putInt(bytes.length);
        buf.put(bytes);
        return buf.array();
    }

    private void sendManagerFrame(byte type, byte[] payload) throws IOException {
        ByteBuffer buf = ByteBuffer.allocate(MANAGER_HEADER_SIZE + payload.length).order(ByteOrder.LITTLE_ENDIAN);
        buf.putInt(MANAGER_PROTOCOL_MAGIC);
        buf.putShort((short) MANAGER_PROTOCOL_VERSION);
        buf.put(type);
        buf.put((byte) 0);
        buf.putInt(payload.length);
        buf.put(payload);
        outputStream.write(buf.array());
        outputStream.flush();
    }

    private ProtocolMessage readManagerFrame() throws IOException {
        byte[] header = new byte[MANAGER_HEADER_SIZE];
        readFully(header);
        ByteBuffer buf = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        int magic = buf.getInt();
        if (magic != MANAGER_PROTOCOL_MAGIC) {
            throw new IOException("Manager frame magic mismatch");
        }
        int version = Short.toUnsignedInt(buf.getShort());
        if (version != MANAGER_PROTOCOL_VERSION) {
            throw new IOException("Manager frame version mismatch");
        }
        byte type = buf.get();
        byte flags = buf.get();
        int length = buf.getInt();
        if (length > MANAGER_MAX_PAYLOAD_SIZE) {
            throw new IOException("Manager payload too large");
        }
        byte[] payload = new byte[length];
        if (length > 0) {
            readFully(payload);
        }
        return new ProtocolMessage(type, flags, length, 0, new byte[16], 0L, payload);
    }

    private void performManagerConnect() throws IOException, SQLException {
        String token = props.getManagerAuthToken() != null ? props.getManagerAuthToken() : "";
        if (token.isEmpty()) {
            throw createSQLException("manager_proxy mode requires manager_auth_token", "08001");
        }
        String managerUser = (props.getManagerUsername() != null && !props.getManagerUsername().isEmpty())
            ? props.getManagerUsername()
            : (props.getUser() != null && !props.getUser().isEmpty() ? props.getUser() : "admin");
        String managerDatabase = (props.getManagerDatabase() != null && !props.getManagerDatabase().isEmpty())
            ? props.getManagerDatabase()
            : (props.getDatabase() != null ? props.getDatabase() : "");
        String managerProfile = (props.getManagerConnectionProfile() != null && !props.getManagerConnectionProfile().isEmpty())
            ? props.getManagerConnectionProfile()
            : "SBsql";
        String managerIntent = (props.getManagerClientIntent() != null && !props.getManagerClientIntent().isEmpty())
            ? props.getManagerClientIntent()
            : "SBsql";

        ByteBuffer hello = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN);
        hello.putShort((short) MCP_PROTOCOL_VERSION);
        hello.putShort((short) props.getManagerClientFlags());
        sendManagerFrame(MCP_MSG_HELLO, hello.array());
        ProtocolMessage msg = readManagerFrame();
        if (msg.type != MCP_MSG_STATUS_RESPONSE) {
            throw createSQLException("Expected MCP hello status response", "08P01");
        }

        ByteArrayOutputStream authStart = new ByteArrayOutputStream();
        authStart.write(buildLengthPrefixedString(managerUser));
        authStart.write(MCP_AUTH_METHOD_TOKEN);
        if (props.isManagerAuthFastPath()) {
            byte[] tokenBytes = token.getBytes(StandardCharsets.UTF_8);
            ByteBuffer tokenLen = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN);
            tokenLen.putInt(tokenBytes.length);
            authStart.write(tokenLen.array());
            authStart.write(tokenBytes);
        } else {
            authStart.write(new byte[4]);
        }
        sendManagerFrame(MCP_MSG_AUTH_START, authStart.toByteArray());
        msg = readManagerFrame();
        if (msg.type == MCP_MSG_AUTH_CHALLENGE) {
            byte[] tokenBytes = token.getBytes(StandardCharsets.UTF_8);
            ByteBuffer authContinue = ByteBuffer.allocate(4 + tokenBytes.length).order(ByteOrder.LITTLE_ENDIAN);
            authContinue.putInt(tokenBytes.length);
            authContinue.put(tokenBytes);
            sendManagerFrame(MCP_MSG_AUTH_CONTINUE, authContinue.array());
            msg = readManagerFrame();
        }
        if (msg.type != MCP_MSG_AUTH_RESPONSE) {
            throw createSQLException("Expected MCP auth response", "08P01");
        }
        if (msg.payload.length < 1 + 4 + 256) {
            throw createSQLException("Truncated MCP auth response", "08P01");
        }
        if (msg.payload[0] != 0) {
            String err = new String(Arrays.copyOfRange(msg.payload, 5, 261), StandardCharsets.UTF_8).replace("\0", "").trim();
            if (err.isEmpty()) {
                err = "MCP authentication failed";
            }
            throw createSQLException(err, "28000");
        }
        resolvedAuthContext.setManagerAuthenticated(true);

        ByteArrayOutputStream dbConnect = new ByteArrayOutputStream();
        dbConnect.write("MCP1".getBytes(StandardCharsets.US_ASCII));
        dbConnect.write(buildLengthPrefixedString(managerDatabase));
        dbConnect.write(buildLengthPrefixedString(managerProfile));
        dbConnect.write(buildLengthPrefixedString(managerIntent));
        byte[] nonce = new byte[16];
        new java.security.SecureRandom().nextBytes(nonce);
        ByteBuffer nonceLen = ByteBuffer.allocate(2).order(ByteOrder.LITTLE_ENDIAN);
        nonceLen.putShort((short) nonce.length);
        dbConnect.write(nonceLen.array());
        dbConnect.write(nonce);
        sendManagerFrame(MCP_MSG_DB_CONNECT, dbConnect.toByteArray());
        msg = readManagerFrame();
        if (msg.type != MCP_MSG_CONNECT_RESPONSE) {
            throw createSQLException("Expected MCP connect response", "08P01");
        }
        if (msg.payload.length < 1 + 2 + 2 + 16 + 64 + 32) {
            throw createSQLException("Truncated MCP connect response", "08P01");
        }
        if (msg.payload[0] != 0) {
            String err = "MCP database connect failed";
            int errOffset = 1 + 2 + 2 + 16 + 64 + 32;
            if (msg.payload.length >= errOffset + 4) {
                ByteBuffer errLenBuf = ByteBuffer.wrap(msg.payload, errOffset, 4).order(ByteOrder.LITTLE_ENDIAN);
                int errLen = errLenBuf.getInt();
                if (msg.payload.length >= errOffset + 4 + errLen) {
                    err = new String(msg.payload, errOffset + 4, errLen, StandardCharsets.UTF_8);
                }
            }
            throw createSQLException(err, "28000");
        }
    }

    private SBAuthProbeResult probeDirectAuthSurface(String resolvedHost, int resolvedPort)
        throws IOException, SQLException {
        sendStartupMessage();
        while (true) {
            ProtocolMessage msg = readMessage();
            switch (msg.type) {
                case MSG_NEGOTIATE_VERSION:
                    continue;
                case MSG_PARAMETER_STATUS: {
                    ParameterStatus status = parseParameterStatus(msg.payload);
                    handleParameterStatus(status);
                    continue;
                }
                case MSG_AUTH_REQUEST: {
                    AuthRequest request = parseAuthRequest(msg.payload);
                    SBAuthMethodSurface surface = describeAuthMethod(request.method);
                    List<SBAuthMethodSurface> admitted = surface == null
                        ? Collections.emptyList()
                        : Collections.singletonList(surface);
                    return new SBAuthProbeResult(
                        true,
                        "direct",
                        resolvedHost,
                        resolvedPort,
                        request.method,
                        authMethodName(request.method),
                        authPluginIdForMethod(request.method),
                        authMethodBrokerRequired(request.method),
                        additionalContinuationPossible(request.method),
                        admitted
                    );
                }
                case MSG_AUTH_OK:
                case MSG_READY:
                    return new SBAuthProbeResult(
                        true,
                        "direct",
                        resolvedHost,
                        resolvedPort,
                        AUTH_OK,
                        authMethodName(AUTH_OK),
                        authPluginIdForMethod(AUTH_OK),
                        false,
                        false,
                        Collections.emptyList()
                    );
                case MSG_ERROR: {
                    ProtocolError error = parseErrorMessage(msg.payload);
                    throw createSQLException(buildErrorMessage(error), error.sqlState != null ? error.sqlState : "28000");
                }
                default:
                    continue;
            }
        }
    }

    private SBAuthProbeResult probeManagerAuthSurface(String resolvedHost, int resolvedPort)
        throws IOException, SQLException {
        String managerUser = (props.getManagerUsername() != null && !props.getManagerUsername().isEmpty())
            ? props.getManagerUsername()
            : (props.getUser() != null && !props.getUser().isEmpty() ? props.getUser() : "admin");

        ByteBuffer hello = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN);
        hello.putShort((short) MCP_PROTOCOL_VERSION);
        hello.putShort((short) props.getManagerClientFlags());
        sendManagerFrame(MCP_MSG_HELLO, hello.array());
        ProtocolMessage msg = readManagerFrame();
        if (msg.type != MCP_MSG_STATUS_RESPONSE) {
            throw createSQLException("Expected MCP hello status response", "08P01");
        }

        ByteArrayOutputStream authStart = new ByteArrayOutputStream();
        authStart.write(buildLengthPrefixedString(managerUser));
        authStart.write(MCP_AUTH_METHOD_TOKEN);
        authStart.write(new byte[4]);
        sendManagerFrame(MCP_MSG_AUTH_START, authStart.toByteArray());
        msg = readManagerFrame();
        if (msg.type != MCP_MSG_AUTH_CHALLENGE
            && msg.type != MCP_MSG_AUTH_RESPONSE
            && msg.type != MCP_MSG_STATUS_RESPONSE) {
            if (msg.payload.length >= 1 && msg.payload[0] != 0) {
                throw createSQLException("MCP authentication probe failed", "28000");
            }
            throw createSQLException("Expected MCP auth challenge or auth response", "08P01");
        }

        SBAuthMethodSurface tokenSurface = describeAuthMethod(AUTH_TOKEN);
        return new SBAuthProbeResult(
            true,
            "manager_proxy",
            resolvedHost,
            resolvedPort,
            AUTH_TOKEN,
            authMethodName(AUTH_TOKEN),
            authPluginIdForMethod(AUTH_TOKEN),
            false,
            msg.type == MCP_MSG_AUTH_CHALLENGE,
            tokenSurface == null ? Collections.emptyList() : Collections.singletonList(tokenSurface)
        );
    }

    private void sendStartupMessage() throws IOException {
        Map<String, String> params = new LinkedHashMap<>();
        params.put("database", props.getDatabase() != null ? props.getDatabase() : "");
        params.put("user", props.getUser() != null ? props.getUser() : "");
        if (props.getRole() != null && !props.getRole().isEmpty()) {
            params.put("role", props.getRole());
        }
        if (props.getApplicationName() != null) {
            params.put("application_name", props.getApplicationName());
        }
        params.put("client_flags", String.valueOf(props.getConnectClientFlags()));
        if (props.getAuthMethodId() != null && !props.getAuthMethodId().isEmpty()) {
            params.put("auth_method_id", props.getAuthMethodId());
        }
        if (props.getAuthMethodPayload() != null && !props.getAuthMethodPayload().isEmpty()) {
            params.put("auth_method_payload", props.getAuthMethodPayload());
        }
        if (props.getAuthPayloadJson() != null && !props.getAuthPayloadJson().isEmpty()) {
            params.put("auth_payload_json", props.getAuthPayloadJson());
        }
        if (props.getAuthPayloadB64() != null && !props.getAuthPayloadB64().isEmpty()) {
            params.put("auth_payload_b64", props.getAuthPayloadB64());
        }
        if (props.getAuthProviderProfile() != null && !props.getAuthProviderProfile().isEmpty()) {
            params.put("auth_provider_profile", props.getAuthProviderProfile());
        }
        if (props.getAuthRequiredMethods() != null && !props.getAuthRequiredMethods().isEmpty()) {
            params.put("auth_required_methods", props.getAuthRequiredMethods());
        }
        if (props.getAuthForbiddenMethods() != null && !props.getAuthForbiddenMethods().isEmpty()) {
            params.put("auth_forbidden_methods", props.getAuthForbiddenMethods());
        }
        if (props.isAuthRequireChannelBinding()) {
            params.put("auth_require_channel_binding", "1");
        }
        if (props.getWorkloadIdentityToken() != null && !props.getWorkloadIdentityToken().isEmpty()) {
            params.put("workload_identity_token", props.getWorkloadIdentityToken());
        }
        if (props.getProxyPrincipalAssertion() != null && !props.getProxyPrincipalAssertion().isEmpty()) {
            params.put("proxy_principal_assertion", props.getProxyPrincipalAssertion());
        }

        long features = 0;
        if ("zstd".equalsIgnoreCase(props.getCompression())) {
            features |= FEATURE_COMPRESSION;
        }
        if (props.isBinaryTransfer()) {
            features |= FEATURE_STREAMING;
        }

        byte[] payload = buildStartupPayload(features, params);
        sendMessage(MSG_STARTUP, payload, (byte) 0, true);
    }

    private void handleAuthentication() throws IOException, SQLException {
        SBScramClient scram = null;
        int activeAuthMethod = AUTH_OK;
        while (true) {
            ProtocolMessage msg = readMessage();
            switch (msg.type) {
                case MSG_NEGOTIATE_VERSION:
                    continue;
                case MSG_AUTH_REQUEST: {
                    AuthRequest request = parseAuthRequest(msg.payload);
                    activeAuthMethod = request.method;
                    resolvedAuthContext.setResolvedMethodCode(request.method);
                    resolvedAuthContext.setResolvedMethodName(authMethodName(request.method));
                    resolvedAuthContext.setResolvedAuthPluginId(authPluginIdForMethod(request.method));
                    if (request.method == AUTH_OK) {
                        continue;
                    }
                    if (request.method == AUTH_PASSWORD) {
                        byte[] response = props.getPassword() != null
                            ? props.getPassword().getBytes(StandardCharsets.UTF_8) : new byte[0];
                        sendMessage(MSG_AUTH_RESPONSE, response, (byte) 0, true);
                        continue;
                    }
                    if (request.method == AUTH_SCRAM_SHA_256 || request.method == AUTH_SCRAM_SHA_512) {
                        if (scram == null) {
                            scram = new SBScramClient(props.getUser(), scramAlgorithmForMethod(request.method));
                        }
                        String clientFirst = scram.getClientFirstMessage();
                        sendMessage(MSG_AUTH_RESPONSE, clientFirst.getBytes(StandardCharsets.UTF_8), (byte) 0, true);
                        continue;
                    }
                    if (request.method == AUTH_TOKEN) {
                        sendMessage(MSG_AUTH_RESPONSE, resolveTokenAuthPayload(), (byte) 0, true);
                        continue;
                    }
                    if (request.method == AUTH_MD5) {
                        throw createSQLException(
                            "admitted auth method MD5 is not implemented in the JDBC lane",
                            "0A000");
                    }
                    if (request.method == AUTH_PEER) {
                        throw createSQLException(
                            "PEER authentication requires broker or platform assistance in the JDBC lane",
                            "0A000");
                    }
                    if (request.method == AUTH_REATTACH) {
                        throw createSQLException(
                            "REATTACH authentication negotiation is not executable through the generic JDBC auth lane",
                            "0A000");
                    }
                    throw createSQLException(
                        "admitted auth method " + authMethodName(request.method) + " requires broker or external ceremony support",
                        "0A000");
                }
                case MSG_AUTH_CONTINUE: {
                    AuthContinue cont = parseAuthContinue(msg.payload);
                    if ((cont.method == AUTH_SCRAM_SHA_256 || cont.method == AUTH_SCRAM_SHA_512) && scram != null) {
                        String clientFinal = scram.handleServerFirst(
                            new String(cont.data, StandardCharsets.UTF_8), props.getPassword());
                        sendMessage(MSG_AUTH_RESPONSE, clientFinal.getBytes(StandardCharsets.UTF_8), (byte) 0, true);
                        continue;
                    }
                    if (cont.method == AUTH_TOKEN) {
                        sendMessage(MSG_AUTH_RESPONSE, resolveTokenAuthPayload(), (byte) 0, true);
                        continue;
                    }
                    throw createSQLException(
                        "admitted auth continuation " + authMethodName(cont.method) + " is not implemented in the JDBC lane",
                        "0A000");
                }
                case MSG_AUTH_OK: {
                    AuthOk ok = parseAuthOk(msg.payload);
                    attachmentId = msg.attachmentId;
                    applyRuntimeTxnId(msg.txnId);
                    if (scram != null && ok.serverInfo.length > 0) {
                        String info = new String(ok.serverInfo, StandardCharsets.UTF_8);
                        if (info.startsWith("v=")) {
                            scram.verifyServerFinal(info);
                        }
                    }
                    if (resolvedAuthContext.getResolvedMethodCode() == AUTH_OK && activeAuthMethod == AUTH_OK) {
                        resolvedAuthContext.setResolvedMethodCode(AUTH_OK);
                        resolvedAuthContext.setResolvedMethodName(authMethodName(AUTH_OK));
                        resolvedAuthContext.setResolvedAuthPluginId(authPluginIdForMethod(AUTH_OK));
                    }
                    continue;
                }
                case MSG_PARAMETER_STATUS: {
                    ParameterStatus status = parseParameterStatus(msg.payload);
                    handleParameterStatus(status);
                    continue;
                }
                case MSG_READY: {
                    ReadyStatus ready = parseReady(msg.payload);
                    applyRuntimeReadyState(ready);
                    resolvedAuthContext.setAttached(true);
                    return;
                }
                case MSG_ERROR: {
                    ProtocolError error = parseErrorMessage(msg.payload);
                    throw createSQLException(buildErrorMessage(error), error.sqlState != null ? error.sqlState : "28000");
                }
                default:
                    continue;
            }
        }
    }

    private void handleParameterStatus(ParameterStatus status) {
        serverParameters.put(status.name, status.value);
        if ("attachment_id".equalsIgnoreCase(status.name)) {
            byte[] parsed = parseUuidBytes(status.value);
            if (parsed != null) {
                attachmentId = parsed;
            }
        }
        if ("current_txn_id".equalsIgnoreCase(status.name)) {
            try {
                applyRuntimeTxnId(Long.parseLong(status.value.trim()));
            } catch (NumberFormatException ignore) {
                // Ignore invalid txn id.
            }
        }
    }

    private String authMethodName(int method) {
        return switch (method) {
            case AUTH_OK -> "OK";
            case AUTH_PASSWORD -> "PASSWORD";
            case AUTH_MD5 -> "MD5";
            case AUTH_SCRAM_SHA_256 -> "SCRAM_SHA_256";
            case AUTH_SCRAM_SHA_512 -> "SCRAM_SHA_512";
            case AUTH_TOKEN -> "TOKEN";
            case AUTH_PEER -> "PEER";
            case AUTH_REATTACH -> "REATTACH";
            default -> "";
        };
    }

    private String authPluginIdForMethod(int method) {
        if (props.getAuthMethodId() != null && !props.getAuthMethodId().isBlank()) {
            return props.getAuthMethodId().trim();
        }
        return switch (method) {
            case AUTH_PASSWORD -> "scratchbird.auth.password_compat";
            case AUTH_MD5 -> "scratchbird.auth.md5_legacy";
            case AUTH_SCRAM_SHA_256 -> "scratchbird.auth.scram_sha_256";
            case AUTH_SCRAM_SHA_512 -> "scratchbird.auth.scram_sha_512";
            case AUTH_TOKEN -> "scratchbird.auth.authkey_token";
            case AUTH_PEER -> "scratchbird.auth.peer_uid";
            case AUTH_REATTACH -> "scratchbird.auth.reattach";
            default -> "";
        };
    }

    private boolean authMethodExecutableLocally(int method) {
        return method == AUTH_PASSWORD
            || method == AUTH_SCRAM_SHA_256
            || method == AUTH_SCRAM_SHA_512
            || method == AUTH_TOKEN;
    }

    private boolean authMethodBrokerRequired(int method) {
        return method == AUTH_PEER;
    }

    private boolean additionalContinuationPossible(int method) {
        return method == AUTH_SCRAM_SHA_256
            || method == AUTH_SCRAM_SHA_512
            || method == AUTH_TOKEN
            || method == AUTH_PEER;
    }

    private SBAuthMethodSurface describeAuthMethod(int method) {
        String name = authMethodName(method);
        if (name.isEmpty()) {
            return null;
        }
        return new SBAuthMethodSurface(
            method,
            name,
            authPluginIdForMethod(method),
            authMethodExecutableLocally(method),
            authMethodBrokerRequired(method)
        );
    }

    private SBScramClient.Algorithm scramAlgorithmForMethod(int method) throws SQLException {
        if (method == AUTH_SCRAM_SHA_256) {
            return SBScramClient.Algorithm.SHA_256;
        }
        if (method == AUTH_SCRAM_SHA_512) {
            return SBScramClient.Algorithm.SHA_512;
        }
        throw createSQLException("Unsupported SCRAM method " + authMethodName(method), "0A000");
    }

    private byte[] resolveTokenAuthPayload() throws SQLException {
        if (props.getAuthToken() != null && !props.getAuthToken().isBlank()) {
            return props.getAuthToken().getBytes(StandardCharsets.UTF_8);
        }
        if (props.getAuthMethodPayload() != null && !props.getAuthMethodPayload().isBlank()) {
            return props.getAuthMethodPayload().getBytes(StandardCharsets.UTF_8);
        }
        if (props.getAuthPayloadB64() != null && !props.getAuthPayloadB64().isBlank()) {
            try {
                return Base64.getDecoder().decode(props.getAuthPayloadB64());
            } catch (IllegalArgumentException ex) {
                throw createSQLException("invalid auth_payload_b64 encoding", "22023", ex);
            }
        }
        if (props.getAuthPayloadJson() != null && !props.getAuthPayloadJson().isBlank()) {
            return props.getAuthPayloadJson().getBytes(StandardCharsets.UTF_8);
        }
        if (props.getWorkloadIdentityToken() != null && !props.getWorkloadIdentityToken().isBlank()) {
            return props.getWorkloadIdentityToken().getBytes(StandardCharsets.UTF_8);
        }
        if (props.getProxyPrincipalAssertion() != null && !props.getProxyPrincipalAssertion().isBlank()) {
            return props.getProxyPrincipalAssertion().getBytes(StandardCharsets.UTF_8);
        }
        throw createSQLException(
            "TOKEN authentication requires auth_token, auth_method_payload, auth_payload_json, auth_payload_b64, workload_identity_token, or proxy_principal_assertion",
            "28000");
    }

    private boolean handleAsyncMessage(ProtocolMessage msg) throws SQLException {
        switch (msg.type) {
            case MSG_PARAMETER_STATUS: {
                handleParameterStatus(parseParameterStatus(msg.payload));
                return true;
            }
            case MSG_NOTIFICATION: {
                NotificationMessage notice = parseNotification(msg.payload);
                for (NotificationListener listener : notificationListeners) {
                    listener.onNotification(notice);
                }
                return true;
            }
            case MSG_QUERY_PLAN: {
                lastPlan = parseQueryPlan(msg.payload);
                return true;
            }
            case MSG_SBLR_COMPILED: {
                lastSblr = parseSblrCompiled(msg.payload);
                return true;
            }
            case MSG_TXN_STATUS: {
                TxnStatus status = parseTxnStatus(msg.payload);
                if (status.status == 'T') {
                    applyRuntimeTxnId(status.txnId);
                } else {
                    clearTransactionState();
                }
                return true;
            }
            default:
                return false;
        }
    }

    private SBQueryResult readQueryResult() throws IOException, SQLException {
        SBQueryResult result = new SBQueryResult();
        List<SBColumnInfo> columns = new ArrayList<>();
        List<Object[]> rows = new ArrayList<>();
        long updateCount = -1;
        String commandTag = null;
        boolean responseStarted = false;
        boolean ignoredStrayReady = false;

        while (true) {
            ProtocolMessage msg = readMessage();
            if (handleAsyncMessage(msg)) {
                continue;
            }
            switch (msg.type) {
                case MSG_ROW_DESCRIPTION:
                    responseStarted = true;
                    columns = parseRowDescription(msg.payload);
                    result.setColumns(columns);
                    break;
                case MSG_DATA_ROW:
                    responseStarted = true;
                    rows.add(parseDataRow(msg.payload, columns));
                    break;
                case MSG_COMMAND_COMPLETE: {
                    responseStarted = true;
                    CommandComplete complete = parseCommandComplete(msg.payload);
                    commandTag = complete.tag;
                    updateCount = complete.rows;
                    break;
                }
                case MSG_READY: {
                    ReadyStatus ready = parseReady(msg.payload);
                    applyRuntimeReadyState(ready);
                    if (!responseStarted && !ignoredStrayReady) {
                        ignoredStrayReady = true;
                        continue;
                    }
                    result.setRows(rows);
                    result.setCommandTag(commandTag);
                    if (updateCount >= 0) {
                        result.setUpdateCount(updateCount);
                    } else {
                        result.setUpdateCount(rows.size());
                    }
                    return result;
                }
                case MSG_ERROR: {
                    ProtocolError error = parseErrorMessage(msg.payload);
                    // Drain back to READY so the connection stays synchronized after
                    // server-side SQL errors and can be safely reused/pool-returned.
                    drainToReadyAfterError();
                    throw createSQLException(buildErrorMessage(error),
                        error.sqlState != null ? error.sqlState : "42000");
                }
                case MSG_NOTICE:
                case MSG_PARSE_COMPLETE:
                case MSG_BIND_COMPLETE:
                case MSG_CLOSE_COMPLETE:
                case MSG_NO_DATA:
                case MSG_PORTAL_SUSPENDED:
                case MSG_EMPTY_QUERY:
                case MSG_TXN_STATUS:
                case MSG_PONG:
                    break;
                default:
                    break;
            }
        }
    }

    private void drainUntilReady() throws IOException, SQLException {
        while (true) {
            ProtocolMessage msg = readMessage();
            if (handleAsyncMessage(msg)) {
                continue;
            }
            switch (msg.type) {
                case MSG_READY: {
                    ReadyStatus ready = parseReady(msg.payload);
                    applyRuntimeReadyState(ready);
                    return;
                }
                case MSG_ERROR: {
                    ProtocolError error = parseErrorMessage(msg.payload);
                    throw createSQLException(buildErrorMessage(error),
                        error.sqlState != null ? error.sqlState : "42000");
                }
                default:
                    break;
            }
        }
    }

    private void drainToReadyAfterError() {
        try {
            while (true) {
                ProtocolMessage msg = readMessage();
                try {
                    if (handleAsyncMessage(msg)) {
                        continue;
                    }
                } catch (SQLException ignored) {
                    // Ignore async parsing errors while trying to resynchronize.
                }
                if (msg.type == MSG_READY) {
                    ReadyStatus ready = parseReady(msg.payload);
                    applyRuntimeReadyState(ready);
                    return;
                }
                if (msg.type == MSG_ERROR) {
                    // Continue draining until READY.
                    continue;
                }
            }
        } catch (IOException ignored) {
            // Connection is likely no longer usable; caller will surface the SQL error.
        }
    }

    private void sendSimpleQuery(String sql, int maxRows, int timeoutMs, int extraFlags) throws IOException {
        int flags = props.isBinaryTransfer() ? QUERY_FLAG_BINARY_RESULT : 0;
        flags |= extraFlags;
        byte[] payload = buildQueryPayload(sql, flags, maxRows, timeoutMs);
        lastPlan = null;
        lastSblr = null;
        lastQuerySequence = sequence;
        sendMessage(MSG_QUERY, payload, (byte) 0, false);
    }

    private void sendExtendedQuery(String sql, List<Object> params, List<Integer> paramTypes,
                                   int maxRows) throws IOException, SQLException {
        List<SBTypeCodec.ParamEncoding> encoded = new ArrayList<>();
        List<Integer> oids = new ArrayList<>();
        for (int i = 0; i < params.size(); i++) {
            Integer sqlType = (paramTypes != null && i < paramTypes.size()) ? paramTypes.get(i) : null;
            SBTypeCodec.ParamEncoding enc = SBTypeCodec.encodeParam(params.get(i), sqlType);
            encoded.add(enc);
            oids.add(enc.getOid());
        }

        String statementName = getOrPrepareStatement(sql, oids);
        int[] resultFormats = props.isBinaryTransfer() ? new int[]{SBTypeCodec.FORMAT_BINARY} : new int[0];
        byte[] bindPayload = buildBindPayload("", statementName, encoded, resultFormats);
        sendMessage(MSG_BIND, bindPayload, (byte) 0, false);

        byte[] execPayload = buildExecutePayload("", maxRows);
        lastPlan = null;
        lastSblr = null;
        lastQuerySequence = sequence;
        sendMessage(MSG_EXECUTE, execPayload, (byte) 0, false);
        if (maxRows <= 0) {
            sendMessage(MSG_SYNC, new byte[0], (byte) 0, false);
        }
    }

    private String getOrPrepareStatement(String sql, List<Integer> parameterTypes) throws IOException, SQLException {
        String key = buildPreparedStatementKey(sql, parameterTypes);
        PreparedStatementCacheEntry cached = preparedStatements.get(key);
        if (cached != null) {
            return cached.statementName;
        }

        String statementName = "sb_stmt_" + preparedStatementSequence++;
        byte[] parsePayload = buildParsePayload(statementName, sql, parameterTypes);
        sendMessage(MSG_PARSE, parsePayload, (byte) 0, false);
        if (!isConnected()) {
            int described = describeStatement(statementName);
            if (described >= 0 && described != parameterTypes.size()) {
                throw createSQLException("parameter count mismatch", "07001");
            }
        }
        preparedStatements.put(key, new PreparedStatementCacheEntry(statementName, parameterTypes));
        while (preparedStatements.size() > MAX_PREPARED_STATEMENTS) {
            String oldestKey = preparedStatements.keySet().iterator().next();
            preparedStatements.remove(oldestKey);
        }
        return statementName;
    }

    private String buildPreparedStatementKey(String sql, List<Integer> parameterTypes) {
        StringBuilder key = new StringBuilder(sql.trim());
        for (Integer type : parameterTypes) {
            key.append('|').append(type);
        }
        return key.toString();
    }

    private void clearPreparedStatements() {
        preparedStatements.clear();
    }

    private boolean isRecoverableCachedStatementError(SQLException error) {
        String state = error.getSQLState();
        String message = error.getMessage();
        if (message == null) {
            return false;
        }
        String lower = message.toLowerCase(Locale.ROOT);
        return isRecoverableCachedStatementState(state, lower)
            || isRecoverableCachedStatementMessage(lower);
    }

    private boolean isRecoverableCachedStatementState(String state, String lowerMessage) {
        if (state == null) {
            return false;
        }
        if (!"26000".equals(state) && !"34000".equals(state) && !"42P01".equals(state)
                && !"42P05".equals(state)) {
            return false;
        }
        return lowerMessage.contains("prepared statement")
            || lowerMessage.contains("unknown prepared statement")
            || lowerMessage.contains("portal does not exist")
            || lowerMessage.contains("relation does not exist")
            || lowerMessage.contains("cache lookup failed for");
    }

    private boolean isRecoverableCachedStatementMessage(String lowerMessage) {
        return lowerMessage.contains("prepared statement")
            || lowerMessage.contains("unknown prepared statement")
            || lowerMessage.contains("cached plan")
            || lowerMessage.contains("cachedplan")
            || lowerMessage.contains("invalid prepared statement")
            || lowerMessage.contains("relation does not exist")
            || lowerMessage.contains("portal does not exist")
            || lowerMessage.contains("cache lookup failed for");
    }

    private boolean isSchemaMutation(String sql) {
        if (sql == null || sql.isEmpty()) {
            return false;
        }
        int index = 0;
        int length = sql.length();
        while (index < length) {
            char ch = sql.charAt(index);
            if (Character.isWhitespace(ch)) {
                index++;
                continue;
            }
            if (ch == '-' && index + 1 < length && sql.charAt(index + 1) == '-') {
                index += 2;
                while (index < length && sql.charAt(index) != '\n') {
                    index++;
                }
                continue;
            }
            if (ch == '/' && index + 1 < length && sql.charAt(index + 1) == '*') {
                index += 2;
                while (index + 1 < length && !(sql.charAt(index) == '*' && sql.charAt(index + 1) == '/')) {
                    index++;
                }
                if (index + 1 < length) {
                    index += 2;
                }
                continue;
            }
            break;
        }
        if (index >= length) {
            return false;
        }

        int tokenStart = index;
        while (index < length && (Character.isLetterOrDigit(sql.charAt(index)) || sql.charAt(index) == '_')) {
            index++;
        }
        String keyword = sql.substring(tokenStart, index).toUpperCase(Locale.ROOT);
        return "CREATE".equals(keyword)
            || "ALTER".equals(keyword)
            || "DROP".equals(keyword)
            || "TRUNCATE".equals(keyword)
            || "COMMENT".equals(keyword)
            || "ANALYZE".equals(keyword);
    }

    private byte[] buildStartupPayload(long features, Map<String, String> params) {
        byte[] paramBytes = buildParamList(params);
        ByteBuffer buf = ByteBuffer.allocate(2 + 2 + 8 + paramBytes.length).order(ByteOrder.LITTLE_ENDIAN);
        buf.put((byte) PROTOCOL_VERSION_MAJOR);
        buf.put((byte) PROTOCOL_VERSION_MINOR);
        buf.putShort((short) 0);
        buf.putLong(features);
        buf.put(paramBytes);
        return buf.array();
    }

    private byte[] buildParamList(Map<String, String> params) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        try {
            for (Map.Entry<String, String> entry : params.entrySet()) {
                out.write(entry.getKey().getBytes(StandardCharsets.UTF_8));
                out.write(0);
                out.write(entry.getValue().getBytes(StandardCharsets.UTF_8));
                out.write(0);
            }
            out.write(0);
        } catch (IOException e) {
            // ByteArrayOutputStream does not throw
        }
        return out.toByteArray();
    }

    private byte[] buildQueryPayload(String sql, int flags, int maxRows, int timeoutMs) {
        byte[] sqlBytes = sql.getBytes(StandardCharsets.UTF_8);
        ByteBuffer buf = ByteBuffer.allocate(12 + sqlBytes.length + 1).order(ByteOrder.LITTLE_ENDIAN);
        buf.putInt(flags);
        buf.putInt(maxRows);
        buf.putInt(timeoutMs);
        buf.put(sqlBytes);
        buf.put((byte) 0);
        return buf.array();
    }

    private byte[] buildParsePayload(String statementName, String sql, List<Integer> paramTypes) {
        byte[] nameBytes = statementName.getBytes(StandardCharsets.UTF_8);
        byte[] sqlBytes = sql.getBytes(StandardCharsets.UTF_8);
        int count = paramTypes != null ? paramTypes.size() : 0;
        int length = 4 + nameBytes.length + 4 + sqlBytes.length + 2 + 2 + (count * 4);
        ByteBuffer buf = ByteBuffer.allocate(length).order(ByteOrder.LITTLE_ENDIAN);
        buf.putInt(nameBytes.length);
        buf.put(nameBytes);
        buf.putInt(sqlBytes.length);
        buf.put(sqlBytes);
        buf.putShort((short) count);
        buf.putShort((short) 0);
        if (paramTypes != null) {
            for (int oid : paramTypes) {
                buf.putInt(oid);
            }
        }
        return buf.array();
    }

    private byte[] buildDescribePayload(byte describeType, String name) {
        byte[] nameBytes = name.getBytes(StandardCharsets.UTF_8);
        ByteBuffer buf = ByteBuffer.allocate(8 + nameBytes.length).order(ByteOrder.LITTLE_ENDIAN);
        buf.put(describeType);
        buf.put(new byte[]{0, 0, 0});
        buf.putInt(nameBytes.length);
        buf.put(nameBytes);
        return buf.array();
    }

    private byte[] buildClosePayload(byte closeType, String name) {
        byte[] nameBytes = name.getBytes(StandardCharsets.UTF_8);
        ByteBuffer buf = ByteBuffer.allocate(8 + nameBytes.length).order(ByteOrder.LITTLE_ENDIAN);
        buf.put(closeType);
        buf.put(new byte[]{0, 0, 0});
        buf.putInt(nameBytes.length);
        buf.put(nameBytes);
        return buf.array();
    }

    private byte[] buildBindPayload(String portalName, String statementName,
                                    List<SBTypeCodec.ParamEncoding> params, int[] resultFormats) {
        byte[] portalBytes = portalName.getBytes(StandardCharsets.UTF_8);
        byte[] stmtBytes = statementName.getBytes(StandardCharsets.UTF_8);
        int paramCount = params != null ? params.size() : 0;

        int length = 4 + portalBytes.length + 4 + stmtBytes.length;
        length += 2 + (paramCount * 2);
        length += 2 + 2;
        for (SBTypeCodec.ParamEncoding param : params) {
            length += 4;
            if (!param.isNull() && param.getData() != null) {
                length += param.getData().length;
            }
        }
        length += 2 + (resultFormats != null ? resultFormats.length * 2 : 0);

        ByteBuffer buf = ByteBuffer.allocate(length).order(ByteOrder.LITTLE_ENDIAN);
        buf.putInt(portalBytes.length);
        buf.put(portalBytes);
        buf.putInt(stmtBytes.length);
        buf.put(stmtBytes);
        buf.putShort((short) paramCount);
        for (SBTypeCodec.ParamEncoding param : params) {
            buf.putShort((short) param.getFormat());
        }
        buf.putShort((short) paramCount);
        buf.putShort((short) 0);
        for (SBTypeCodec.ParamEncoding param : params) {
            if (param.isNull()) {
                buf.putInt(-1);
                continue;
            }
            byte[] data = param.getData() != null ? param.getData() : new byte[0];
            buf.putInt(data.length);
            buf.put(data);
        }
        if (resultFormats == null) {
            buf.putShort((short) 0);
        } else {
            buf.putShort((short) resultFormats.length);
            for (int fmt : resultFormats) {
                buf.putShort((short) fmt);
            }
        }
        return buf.array();
    }

    private byte[] buildExecutePayload(String portalName, int maxRows) {
        byte[] portalBytes = portalName.getBytes(StandardCharsets.UTF_8);
        ByteBuffer buf = ByteBuffer.allocate(4 + portalBytes.length + 4).order(ByteOrder.LITTLE_ENDIAN);
        buf.putInt(portalBytes.length);
        buf.put(portalBytes);
        buf.putInt(maxRows);
        return buf.array();
    }

    private byte[] buildCancelPayload(int cancelType, int targetSequence) {
        ByteBuffer buf = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN);
        buf.putInt(cancelType);
        buf.putInt(targetSequence);
        return buf.array();
    }

    private byte[] buildSblrExecutePayload(long sblrHash, byte[] bytecode, List<SBTypeCodec.ParamEncoding> params) {
        byte[] safeBytecode = bytecode != null ? bytecode : new byte[0];
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        ByteBuffer header = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
        header.putLong(sblrHash);
        header.putInt(safeBytecode.length);
        header.putShort((short) params.size());
        header.putShort((short) 0);
        out.writeBytes(header.array());
        out.writeBytes(safeBytecode);
        for (SBTypeCodec.ParamEncoding param : params) {
            byte[] data = param.getData();
            if (param.isNull() || data == null) {
                out.writeBytes(ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(-1).array());
            } else {
                ByteBuffer len = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN);
                len.putInt(data.length);
                out.writeBytes(len.array());
                out.writeBytes(data);
            }
        }
        return out.toByteArray();
    }

    private byte[] buildSubscribePayload(byte subscribeType, String channel, String filterExpr) {
        byte[] channelBytes = channel.getBytes(StandardCharsets.UTF_8);
        byte[] filterBytes = filterExpr != null ? filterExpr.getBytes(StandardCharsets.UTF_8) : new byte[0];
        ByteBuffer buf = ByteBuffer.allocate(8 + channelBytes.length + filterBytes.length).order(ByteOrder.LITTLE_ENDIAN);
        buf.put(subscribeType);
        buf.put(new byte[3]);
        buf.putInt(channelBytes.length);
        buf.put(channelBytes);
        buf.putInt(filterBytes.length);
        buf.put(filterBytes);
        return buf.array();
    }

    private byte[] buildUnsubscribePayload(String channel) {
        byte[] channelBytes = channel.getBytes(StandardCharsets.UTF_8);
        ByteBuffer buf = ByteBuffer.allocate(4 + channelBytes.length).order(ByteOrder.LITTLE_ENDIAN);
        buf.putInt(channelBytes.length);
        buf.put(channelBytes);
        return buf.array();
    }

    private byte[] buildTxnBeginPayload(short flags, byte conflictAction, byte autocommitMode,
                                        byte isolationLevel, byte accessMode, byte deferrable,
                                        byte waitMode, int timeoutMs, byte readCommittedMode) {
        ByteBuffer buf = ByteBuffer
            .allocate((flags & TXN_FLAG_HAS_READ_COMMITTED_MODE) != 0 ? 16 : 12)
            .order(ByteOrder.LITTLE_ENDIAN);
        buf.putShort(flags);
        buf.put(conflictAction);
        buf.put(autocommitMode);
        buf.put(isolationLevel);
        buf.put(accessMode);
        buf.put(deferrable);
        buf.put(waitMode);
        buf.putInt(timeoutMs);
        if ((flags & TXN_FLAG_HAS_READ_COMMITTED_MODE) != 0) {
            buf.put(readCommittedMode);
            buf.put((byte) 0);
            buf.put((byte) 0);
            buf.put((byte) 0);
        }
        return buf.array();
    }

    private byte[] buildTxnCommitPayload(byte flags) {
        return new byte[]{flags, 0, 0, 0};
    }

    private byte[] buildTxnRollbackPayload(byte flags) {
        return new byte[]{flags, 0, 0, 0};
    }

    private byte[] buildTxnSavepointPayload(String name) {
        byte[] nameBytes = name.getBytes(StandardCharsets.UTF_8);
        ByteBuffer buf = ByteBuffer.allocate(4 + nameBytes.length).order(ByteOrder.LITTLE_ENDIAN);
        buf.putInt(nameBytes.length);
        buf.put(nameBytes);
        return buf.array();
    }

    private byte[] buildTxnReleasePayload(String name) {
        return buildTxnSavepointPayload(name);
    }

    private byte[] buildTxnRollbackToPayload(String name) {
        return buildTxnSavepointPayload(name);
    }

    private byte[] buildSetOptionPayload(String name, String value) {
        byte[] nameBytes = name.getBytes(StandardCharsets.UTF_8);
        byte[] valueBytes = value.getBytes(StandardCharsets.UTF_8);
        ByteBuffer buf = ByteBuffer.allocate(8 + nameBytes.length + valueBytes.length).order(ByteOrder.LITTLE_ENDIAN);
        buf.putInt(nameBytes.length);
        buf.put(nameBytes);
        buf.putInt(valueBytes.length);
        buf.put(valueBytes);
        return buf.array();
    }

    private byte[] buildStreamControlPayload(byte controlType, int windowSize, int timeoutMs) {
        ByteBuffer buf = ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN);
        buf.put(controlType);
        buf.put(new byte[3]);
        buf.putInt(windowSize);
        buf.putInt(timeoutMs);
        return buf.array();
    }

    private byte[] buildAttachCreatePayload(String emulationMode, String dbName) {
        byte[] modeBytes = emulationMode.getBytes(StandardCharsets.UTF_8);
        byte[] dbBytes = dbName.getBytes(StandardCharsets.UTF_8);
        ByteBuffer buf = ByteBuffer.allocate(8 + modeBytes.length + dbBytes.length).order(ByteOrder.LITTLE_ENDIAN);
        buf.putInt(modeBytes.length);
        buf.put(modeBytes);
        buf.putInt(dbBytes.length);
        buf.put(dbBytes);
        return buf.array();
    }

    private int sendMessage(byte type, byte[] payload, byte flags, boolean forceZero) throws IOException {
        int seq = sequence++;
        ByteBuffer buf = ByteBuffer.allocate(HEADER_SIZE + payload.length).order(ByteOrder.LITTLE_ENDIAN);
        buf.putInt(PROTOCOL_MAGIC);
        buf.put((byte) PROTOCOL_VERSION_MAJOR);
        buf.put((byte) PROTOCOL_VERSION_MINOR);
        buf.put(type);
        buf.put(flags);
        buf.putInt(payload.length);
        buf.putInt(seq);
        if (forceZero) {
            buf.put(new byte[16]);
            buf.putLong(0);
        } else {
            buf.put(attachmentId);
            buf.putLong(txnId);
        }
        buf.put(payload);
        outputStream.write(buf.array());
        outputStream.flush();
        return seq;
    }

    private ProtocolMessage readMessage() throws IOException {
        byte[] header = new byte[HEADER_SIZE];
        readFully(header);
        ByteBuffer buf = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        int magic = buf.getInt();
        if (magic != PROTOCOL_MAGIC) {
            throw new IOException("Invalid protocol magic");
        }
        int major = buf.get() & 0xff;
        int minor = buf.get() & 0xff;
        if (major != PROTOCOL_VERSION_MAJOR || minor != PROTOCOL_VERSION_MINOR) {
            throw new IOException("Unsupported protocol version");
        }
        byte type = buf.get();
        byte flags = buf.get();
        int length = buf.getInt();
        if (length > MAX_MESSAGE_SIZE) {
            throw new IOException("Message too large");
        }
        int sequence = buf.getInt();
        byte[] attach = new byte[16];
        buf.get(attach);
        long txnId = buf.getLong();
        byte[] payload = new byte[length];
        if (length > 0) {
            readFully(payload);
        }
        return new ProtocolMessage(type, flags, length, sequence, attach, txnId, payload);
    }

    private void readFully(byte[] buffer) throws IOException {
        int offset = 0;
        while (offset < buffer.length) {
            int read = inputStream.read(buffer, offset, buffer.length - offset);
            if (read < 0) {
                throw new EOFException("Connection closed");
            }
            offset += read;
        }
    }

    private List<SBColumnInfo> parseRowDescription(byte[] payload) throws SQLException {
        ByteBuffer buf = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        int count = Short.toUnsignedInt(buf.getShort());
        buf.getShort();
        List<SBColumnInfo> columns = new ArrayList<>(count);
        for (int i = 0; i < count; i++) {
            int nameLen = buf.getInt();
            byte[] nameBytes = new byte[nameLen];
            buf.get(nameBytes);
            String name = new String(nameBytes, StandardCharsets.UTF_8);
            int tableOid = buf.getInt();
            int columnIndex = Short.toUnsignedInt(buf.getShort());
            int typeOid = buf.getInt();
            short typeSize = buf.getShort();
            int typeModifier = buf.getInt();
            short format = (short) (buf.get() & 0xff);
            boolean nullable = buf.get() == 1;
            buf.getShort();

            SBColumnInfo col = new SBColumnInfo();
            col.setName(name);
            col.setTableOid(tableOid);
            col.setColumnNumber((short) columnIndex);
            col.setTypeOid(typeOid);
            col.setTypeSize(typeSize);
            col.setTypeModifier(typeModifier);
            col.setFormatCode(format);
            col.setNullable(nullable);
            columns.add(col);
        }
        return columns;
    }

    private Object[] parseDataRow(byte[] payload, List<SBColumnInfo> columns) throws SQLException {
        ByteBuffer buf = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        int count = Short.toUnsignedInt(buf.getShort());
        int nullBytes = Short.toUnsignedInt(buf.getShort());
        byte[] nullBitmap = new byte[nullBytes];
        buf.get(nullBitmap);
        Object[] row = new Object[count];
        for (int i = 0; i < count; i++) {
            boolean isNull = false;
            if (nullBytes > 0) {
                isNull = (nullBitmap[i / 8] & (1 << (i % 8))) != 0;
            }
            if (isNull) {
                row[i] = null;
                continue;
            }
            int len = buf.getInt();
            if (len < 0) {
                row[i] = null;
                continue;
            }
            byte[] data = new byte[len];
            buf.get(data);
            SBColumnInfo col = columns.get(i);
            row[i] = SBTypeCodec.decodeValue(col.getTypeOid(), data, col.getFormatCode());
        }
        return row;
    }

    private CommandComplete parseCommandComplete(byte[] payload) {
        if (payload.length < 20) {
            return new CommandComplete(0, 0, "");
        }
        ByteBuffer buf = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        buf.get();
        buf.get(new byte[3]);
        long rows = buf.getLong();
        long lastId = buf.getLong();
        byte[] tagBytes = new byte[payload.length - 20];
        buf.get(tagBytes);
        int nullIdx = -1;
        for (int i = 0; i < tagBytes.length; i++) {
            if (tagBytes[i] == 0) {
                nullIdx = i;
                break;
            }
        }
        String tag = nullIdx >= 0
            ? new String(tagBytes, 0, nullIdx, StandardCharsets.UTF_8)
            : new String(tagBytes, StandardCharsets.UTF_8);
        return new CommandComplete(rows, lastId, tag);
    }

    private NotificationMessage parseNotification(byte[] payload) throws SQLException {
        if (payload.length < 12) {
            throw createSQLException("Notification truncated", "08P01");
        }
        ByteBuffer buf = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        int processId = buf.getInt();
        int channelLen = buf.getInt();
        if (channelLen < 0 || channelLen > buf.remaining() - 4) {
            throw createSQLException("Notification truncated", "08P01");
        }
        byte[] channelBytes = new byte[channelLen];
        buf.get(channelBytes);
        int payloadLen = buf.getInt();
        if (payloadLen < 0 || payloadLen > buf.remaining()) {
            throw createSQLException("Notification truncated", "08P01");
        }
        byte[] noticePayload = new byte[payloadLen];
        buf.get(noticePayload);
        Character changeType = null;
        Long rowId = null;
        if (buf.remaining() >= 1) {
            changeType = (char) (buf.get() & 0xff);
            if (buf.remaining() >= 8) {
                rowId = buf.getLong();
            }
        }
        return new NotificationMessage(processId, new String(channelBytes, StandardCharsets.UTF_8), noticePayload, changeType, rowId);
    }

    private QueryPlanMessage parseQueryPlan(byte[] payload) throws SQLException {
        if (payload.length < 32) {
            throw createSQLException("Query plan truncated", "08P01");
        }
        ByteBuffer buf = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        int format = buf.getInt();
        int planLen = buf.getInt();
        long planningTimeUs = buf.getLong();
        long estimatedRows = buf.getLong();
        long estimatedCost = buf.getLong();
        if (planLen < 0 || planLen > buf.remaining()) {
            throw createSQLException("Query plan truncated", "08P01");
        }
        byte[] plan = new byte[planLen];
        buf.get(plan);
        return new QueryPlanMessage(format, planningTimeUs, estimatedRows, estimatedCost, plan);
    }

    private SblrCompiledMessage parseSblrCompiled(byte[] payload) throws SQLException {
        if (payload.length < 16) {
            throw createSQLException("SBLR compiled truncated", "08P01");
        }
        ByteBuffer buf = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        long hash = buf.getLong();
        int version = buf.getInt();
        int length = buf.getInt();
        if (length < 0 || length > buf.remaining()) {
            throw createSQLException("SBLR compiled truncated", "08P01");
        }
        byte[] bytecode = new byte[length];
        buf.get(bytecode);
        return new SblrCompiledMessage(hash, version, bytecode);
    }

    private byte[] parseUuidBytes(String value) {
        if (value == null) {
            return null;
        }
        String hex = value.replace("-", "").trim();
        if (!hex.matches("^[0-9A-Fa-f]{32}$")) {
            return null;
        }
        byte[] out = new byte[16];
        for (int i = 0; i < 16; i++) {
            int idx = i * 2;
            out[i] = (byte) Integer.parseInt(hex.substring(idx, idx + 2), 16);
        }
        return out;
    }

    private ProtocolError parseErrorMessage(byte[] payload) {
        String severity = null;
        String sqlState = null;
        String message = null;
        String detail = null;
        String hint = null;
        int pos = 0;
        while (pos < payload.length) {
            byte field = payload[pos++];
            if (field == 0) {
                break;
            }
            int start = pos;
            while (pos < payload.length && payload[pos] != 0) {
                pos++;
            }
            if (pos >= payload.length) {
                break;
            }
            String value = new String(payload, start, pos - start, StandardCharsets.UTF_8);
            pos++;
            switch ((char) field) {
                case 'S':
                    severity = value;
                    break;
                case 'C':
                    sqlState = value;
                    break;
                case 'M':
                    message = value;
                    break;
                case 'D':
                    detail = value;
                    break;
                case 'H':
                    hint = value;
                    break;
                default:
                    break;
            }
        }
        return new ProtocolError(severity, sqlState, message, detail, hint);
    }

    private String buildErrorMessage(ProtocolError error) {
        if (error == null) {
            return "query failed";
        }
        StringBuilder sb = new StringBuilder();
        if (error.message != null) {
            sb.append(error.message);
        }
        if (error.detail != null && !error.detail.isEmpty()) {
            if (sb.length() > 0) sb.append("\n");
            sb.append("DETAIL: ").append(error.detail);
        }
        if (error.hint != null && !error.hint.isEmpty()) {
            if (sb.length() > 0) sb.append("\n");
            sb.append("HINT: ").append(error.hint);
        }
        return sb.length() > 0 ? sb.toString() : "query failed";
    }

    private AuthRequest parseAuthRequest(byte[] payload) {
        if (payload.length < 4) {
            return new AuthRequest(AUTH_OK, new byte[0]);
        }
        int method = payload[0] & 0xff;
        byte[] data = Arrays.copyOfRange(payload, 4, payload.length);
        return new AuthRequest(method, data);
    }

    private AuthContinue parseAuthContinue(byte[] payload) {
        if (payload.length < 8) {
            return new AuthContinue(AUTH_OK, 0, new byte[0]);
        }
        int method = payload[0] & 0xff;
        int stage = payload[1] & 0xff;
        int dataLen = ByteBuffer.wrap(payload, 4, 4).order(ByteOrder.LITTLE_ENDIAN).getInt();
        int end = Math.min(payload.length, 8 + dataLen);
        byte[] data = Arrays.copyOfRange(payload, 8, end);
        return new AuthContinue(method, stage, data);
    }

    private AuthOk parseAuthOk(byte[] payload) {
        if (payload.length < 20) {
            return new AuthOk(new byte[16], new byte[0]);
        }
        byte[] sessionId = Arrays.copyOfRange(payload, 0, 16);
        int infoLen = ByteBuffer.wrap(payload, 16, 4).order(ByteOrder.LITTLE_ENDIAN).getInt();
        int end = Math.min(payload.length, 20 + infoLen);
        byte[] serverInfo = Arrays.copyOfRange(payload, 20, end);
        return new AuthOk(sessionId, serverInfo);
    }

    private ParameterStatus parseParameterStatus(byte[] payload) {
        if (payload.length < 8) {
            return new ParameterStatus("", "");
        }
        ByteBuffer buf = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        int nameLen = buf.getInt();
        byte[] nameBytes = new byte[nameLen];
        buf.get(nameBytes);
        int valueLen = buf.getInt();
        byte[] valueBytes = new byte[valueLen];
        buf.get(valueBytes);
        return new ParameterStatus(new String(nameBytes, StandardCharsets.UTF_8),
            new String(valueBytes, StandardCharsets.UTF_8));
    }

    private List<Integer> parseParameterDescription(byte[] payload) throws SQLException {
        if (payload.length < 4) {
            throw createSQLException("Parameter description truncated", "08P01");
        }
        ByteBuffer buf = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        int count = buf.getShort() & 0xffff;
        buf.getShort();
        List<Integer> types = new ArrayList<>(count);
        for (int i = 0; i < count; i++) {
            if (buf.remaining() < 4) {
                throw createSQLException("Parameter description truncated", "08P01");
            }
            types.add(buf.getInt());
        }
        return types;
    }

    private int describeStatement(String name) throws IOException, SQLException {
        byte[] payload = buildDescribePayload((byte) 'S', name);
        sendMessage(MSG_DESCRIBE, payload, (byte) 0, false);
        sendMessage(MSG_SYNC, new byte[0], (byte) 0, false);
        int paramCount = -1;
        while (true) {
            ProtocolMessage msg = readMessage();
            if (handleAsyncMessage(msg)) {
                continue;
            }
            switch (msg.type) {
                case MSG_PARAMETER_DESCRIPTION:
                    paramCount = parseParameterDescription(msg.payload).size();
                    break;
                case MSG_ERROR:
                    ProtocolError error = parseErrorMessage(msg.payload);
                    throw createSQLException(buildErrorMessage(error),
                        error.sqlState != null ? error.sqlState : "42000");
                case MSG_READY:
                    ReadyStatus ready = parseReady(msg.payload);
                    applyRuntimeReadyState(ready);
                    return paramCount;
                default:
                    break;
            }
        }
    }

    private ReadyStatus parseReady(byte[] payload) {
        if (payload.length < 20) {
            return new ReadyStatus((byte) 0, 0, 0);
        }
        ByteBuffer buf = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        byte status = buf.get();
        buf.get(new byte[3]);
        long txn = buf.getLong();
        long visibility = buf.getLong();
        return new ReadyStatus(status, txn, visibility);
    }

    private TxnStatus parseTxnStatus(byte[] payload) {
        if (payload.length < 12) {
            return new TxnStatus((byte) 0, 0);
        }
        ByteBuffer buf = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        byte status = buf.get();
        buf.get(new byte[3]);
        long txn = buf.getLong();
        return new TxnStatus(status, txn);
    }

    private void upgradeToSSL(String sslMode) throws IOException, SQLException {
        SSLSocketFactory factory = createSslContext().getSocketFactory();
        SSLSocket sslSocket = (SSLSocket) factory.createSocket(socket, props.getHost(), props.getPort(), true);
        if ("verify-full".equalsIgnoreCase(sslMode)) {
            SSLParameters params = sslSocket.getSSLParameters();
            params.setEndpointIdentificationAlgorithm("HTTPS");
            sslSocket.setSSLParameters(params);
        }
        sslSocket.startHandshake();
        if (props.getSocketTimeout() > 0) {
            sslSocket.setSoTimeout(props.getSocketTimeout() * 1000);
        }

        socket = sslSocket;
        inputStream = new BufferedInputStream(socket.getInputStream(), 65536);
        outputStream = new BufferedOutputStream(socket.getOutputStream(), 65536);
    }

    private SSLContext createSslContext() throws SQLException {
        try {
            KeyManager[] keyManagers = null;
            TrustManager[] trustManagers = null;

            if (props.getSslRootCert() != null) {
                trustManagers = buildTrustManagers(props.getSslRootCert());
            }

            if (props.getSslCert() != null) {
                keyManagers = buildKeyManagers(props.getSslCert(), props.getSslPassword());
            }

            SSLContext context = SSLContext.getInstance("TLS");
            context.init(keyManagers, trustManagers, null);
            return context;
        } catch (Exception e) {
            throw createSQLException("Failed to initialize SSL context: " + e.getMessage(), "08001", e);
        }
    }

    private TrustManager[] buildTrustManagers(String caPath) throws Exception {
        java.security.cert.CertificateFactory cf =
            java.security.cert.CertificateFactory.getInstance("X.509");
        java.security.cert.Certificate cert;
        try (InputStream in = new FileInputStream(caPath)) {
            cert = cf.generateCertificate(in);
        }
        KeyStore trustStore = KeyStore.getInstance(KeyStore.getDefaultType());
        trustStore.load(null, null);
        trustStore.setCertificateEntry("ca", cert);
        TrustManagerFactory tmf =
            TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
        tmf.init(trustStore);
        return tmf.getTrustManagers();
    }

    private KeyManager[] buildKeyManagers(String keystorePath, String password) throws Exception {
        String lower = keystorePath.toLowerCase();
        String type = (lower.endsWith(".p12") || lower.endsWith(".pfx")) ? "PKCS12" : "JKS";
        KeyStore keyStore = KeyStore.getInstance(type);
        char[] pass = password != null ? password.toCharArray() : new char[0];
        try (InputStream in = new FileInputStream(keystorePath)) {
            keyStore.load(in, pass);
        }
        KeyManagerFactory kmf =
            KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm());
        kmf.init(keyStore, pass);
        return kmf.getKeyManagers();
    }

    private static final class PreparedStatementCacheEntry {
        final String statementName;
        final String sql;
        final List<Integer> parameterTypes;

        PreparedStatementCacheEntry(String statementName, String sql, List<Integer> parameterTypes) {
            this.statementName = statementName;
            this.sql = sql;
            this.parameterTypes = List.copyOf(parameterTypes);
        }

        PreparedStatementCacheEntry(String statementName, List<Integer> parameterTypes) {
            this(statementName, "", parameterTypes);
        }
    }

    private static final class ProtocolMessage {
        final byte type;
        final byte flags;
        final int length;
        final int sequence;
        final byte[] attachmentId;
        final long txnId;
        final byte[] payload;

        ProtocolMessage(byte type, byte flags, int length, int sequence,
                        byte[] attachmentId, long txnId, byte[] payload) {
            this.type = type;
            this.flags = flags;
            this.length = length;
            this.sequence = sequence;
            this.attachmentId = attachmentId;
            this.txnId = txnId;
            this.payload = payload;
        }
    }

    private static final class StreamingCursor implements SBRowStream {
        private final SBProtocolHandler protocol;
        private List<SBColumnInfo> columns = new ArrayList<>();
        private long updateCount = -1;
        private String commandTag;
        private boolean done = false;
        private boolean responseStarted = false;
        private boolean ignoredStrayReady = false;
        private final int pageSize;
        private final ScheduledFuture<?> cancelTask;

        StreamingCursor(SBProtocolHandler protocol, int pageSize, ScheduledFuture<?> cancelTask) {
            this.protocol = protocol;
            this.pageSize = pageSize;
            this.cancelTask = cancelTask;
        }

        @Override
        public Object[] nextRow() throws SQLException {
            if (done) {
                return null;
            }
            while (true) {
                ProtocolMessage msg;
                try {
                    msg = protocol.readMessage();
                } catch (IOException e) {
                    throw protocol.createSQLException("Query execution failed: " + e.getMessage(), "08006", e);
                }
                if (protocol.handleAsyncMessage(msg)) {
                    continue;
                }
                switch (msg.type) {
                    case MSG_ROW_DESCRIPTION:
                        responseStarted = true;
                        columns = protocol.parseRowDescription(msg.payload);
                        break;
                    case MSG_DATA_ROW:
                        responseStarted = true;
                        return protocol.parseDataRow(msg.payload, columns);
                    case MSG_COMMAND_COMPLETE: {
                        responseStarted = true;
                        CommandComplete complete = protocol.parseCommandComplete(msg.payload);
                        commandTag = complete.tag;
                        updateCount = complete.rows;
                        break;
                    }
                    case MSG_PORTAL_SUSPENDED: {
                        protocol.allowPortalResume();
                        protocol.resumeSuspendedPortal(pageSize);
                        break;
                    }
                    case MSG_READY: {
                        ReadyStatus ready = protocol.parseReady(msg.payload);
                        protocol.applyRuntimeReadyState(ready);
                        if (!responseStarted && !ignoredStrayReady) {
                            ignoredStrayReady = true;
                            continue;
                        }
                        done = true;
                        if (cancelTask != null) {
                            cancelTask.cancel(false);
                        }
                        return null;
                    }
                    case MSG_ERROR: {
                        ProtocolError error = protocol.parseErrorMessage(msg.payload);
                        protocol.drainToReadyAfterError();
                        if (cancelTask != null) {
                            cancelTask.cancel(false);
                        }
                        throw protocol.createSQLException(protocol.buildErrorMessage(error),
                            error.sqlState != null ? error.sqlState : "42000");
                    }
                    default:
                        break;
                }
            }
        }

        @Override
        public List<SBColumnInfo> getColumns() {
            return columns;
        }

        @Override
        public long getUpdateCount() {
            return updateCount;
        }

        @Override
        public String getCommandTag() {
            return commandTag;
        }

        @Override
        public boolean isDone() {
            return done;
        }

        @Override
        public void close() throws SQLException {
            if (done) {
                if (cancelTask != null) {
                    cancelTask.cancel(false);
                }
                return;
            }
            try {
                boolean closeSent = false;
                while (!done) {
                    ProtocolMessage msg = protocol.readMessage();
                    if (protocol.handleAsyncMessage(msg)) {
                        continue;
                    }
                    switch (msg.type) {
                        case MSG_ROW_DESCRIPTION:
                            responseStarted = true;
                            columns = protocol.parseRowDescription(msg.payload);
                            break;
                        case MSG_DATA_ROW:
                            responseStarted = true;
                            break;
                        case MSG_COMMAND_COMPLETE: {
                            responseStarted = true;
                            CommandComplete complete = protocol.parseCommandComplete(msg.payload);
                            commandTag = complete.tag;
                            updateCount = complete.rows;
                            break;
                        }
                        case MSG_PORTAL_SUSPENDED:
                            responseStarted = true;
                            if (!closeSent) {
                                protocol.closeStreamingPortal();
                                closeSent = true;
                            }
                            break;
                        case MSG_CLOSE_COMPLETE:
                            break;
                        case MSG_READY: {
                            ReadyStatus ready = protocol.parseReady(msg.payload);
                            protocol.applyRuntimeReadyState(ready);
                            done = true;
                            return;
                        }
                        case MSG_ERROR: {
                            ProtocolError error = protocol.parseErrorMessage(msg.payload);
                            protocol.drainToReadyAfterError();
                            done = true;
                            if (closeSent && protocol.isIgnorablePortalCloseError(error)) {
                                return;
                            }
                            throw protocol.createSQLException(protocol.buildErrorMessage(error),
                                error.sqlState != null ? error.sqlState : "42000");
                        }
                        default:
                            break;
                    }
                }
            } catch (IOException e) {
                throw protocol.createSQLException("Failed to close streaming result: " + e.getMessage(), "08006", e);
            } finally {
                if (cancelTask != null) {
                    cancelTask.cancel(false);
                }
            }
        }
    }

    private boolean isIgnorablePortalCloseError(ProtocolError error) {
        if (error == null) {
            return false;
        }
        String state = error.sqlState;
        String message = buildErrorMessage(error);
        if (message == null) {
            return false;
        }
        String lower = message.toLowerCase(Locale.ROOT);
        return ("26000".equals(state) || "34000".equals(state) || "42P01".equals(state))
            && lower.contains("portal does not exist");
    }

    private static final class AuthRequest {
        final int method;
        final byte[] data;

        AuthRequest(int method, byte[] data) {
            this.method = method;
            this.data = data;
        }
    }

    private static final class AuthContinue {
        final int method;
        final int stage;
        final byte[] data;

        AuthContinue(int method, int stage, byte[] data) {
            this.method = method;
            this.stage = stage;
            this.data = data;
        }
    }

    private static final class AuthOk {
        final byte[] sessionId;
        final byte[] serverInfo;

        AuthOk(byte[] sessionId, byte[] serverInfo) {
            this.sessionId = sessionId;
            this.serverInfo = serverInfo;
        }
    }

    private static final class ParameterStatus {
        final String name;
        final String value;

        ParameterStatus(String name, String value) {
            this.name = name;
            this.value = value;
        }
    }

    private SQLException createSQLException(String message, String sqlState) {
        return createSQLException(message, sqlState, null);
    }

    private SQLException createSQLException(String message, String sqlState, Throwable cause) {
        String state = (sqlState == null || sqlState.isEmpty()) ? "42000" : sqlState;
        SQLException ex;
        ex = mapByExactState(message, state);
        if (ex == null) {
            ex = mapByStateClass(message, state);
        }
        if (ex == null) {
            ex = new SQLException(message, state);
        }
        if (cause != null) {
            ex.initCause(cause);
        }
        return ex;
    }

    private SQLException mapByExactState(String message, String state) {
        return switch (state) {
            case "01000" -> new SQLWarning(message, state);
            case "02000" -> new SQLDataException(message, state);
            case "08001", "08003", "08006" -> new SQLTransientConnectionException(message, state);
            case "08004", "08P01" -> new SQLNonTransientConnectionException(message, state);
            case "0A000" -> new SQLFeatureNotSupportedException(message, state);
            case "22001", "22003", "22007", "22012", "22023", "22P02", "22P03" ->
                new SQLDataException(message, state);
            case "23000", "23502", "23503", "23505", "23514" ->
                new SQLIntegrityConstraintViolationException(message, state);
            case "28000", "28P01" -> new SQLInvalidAuthorizationSpecException(message, state);
            case "40001", "40P01" -> new SQLTransactionRollbackException(message, state);
            case "25P02" -> new SQLTransactionRollbackException(message, state);
            case "25006" -> new SQLNonTransientException(message, state);
            case "34000" -> new SQLNonTransientException(message, state);
            case "3D000" -> new SQLNonTransientException(message, state);
            case "42501" -> new SQLInvalidAuthorizationSpecException(message, state);
            case "42601", "42703", "42704", "42710", "42883", "42P01", "42P07" ->
                new SQLSyntaxErrorException(message, state);
            case "55P03" -> new SQLTransientException(message, state);
            case "55000", "55006" -> new SQLNonTransientException(message, state);
            case "53P00", "53100", "53200", "53300" -> new SQLTransientException(message, state);
            case "54000" -> new SQLNonTransientException(message, state);
            case "57014" -> new SQLTimeoutException(message, state);
            case "57P03" -> new SQLTransientConnectionException(message, state);
            case "57P01" -> new SQLNonTransientConnectionException(message, state);
            case "58000", "XX000" -> new SQLNonTransientException(message, state);
            default -> null;
        };
    }

    private SQLException mapByStateClass(String message, String state) {
        String stateClass = state.substring(0, 2);
        return switch (stateClass) {
            case "01" -> new SQLWarning(message, state);
            case "02" -> new SQLDataException(message, state);
            case "08" -> new SQLTransientConnectionException(message, state);
            case "0A" -> new SQLFeatureNotSupportedException(message, state);
            case "22" -> new SQLDataException(message, state);
            case "23" -> new SQLIntegrityConstraintViolationException(message, state);
            case "25" -> new SQLNonTransientException(message, state);
            case "28" -> new SQLInvalidAuthorizationSpecException(message, state);
            case "34" -> new SQLNonTransientException(message, state);
            case "3D" -> new SQLNonTransientException(message, state);
            case "40" -> new SQLTransactionRollbackException(message, state);
            case "42" -> new SQLSyntaxErrorException(message, state);
            case "53" -> new SQLTransientException(message, state);
            case "54" -> new SQLNonTransientException(message, state);
            case "55" -> new SQLNonTransientException(message, state);
            case "57" -> new SQLNonTransientException(message, state);
            case "58" -> new SQLNonTransientException(message, state);
            default -> null;
        };
    }

    private static final class ReadyStatus {
        final byte status;
        final long txnId;
        final long visibility;

        ReadyStatus(byte status, long txnId, long visibility) {
            this.status = status;
            this.txnId = txnId;
            this.visibility = visibility;
        }
    }

    private static final class TxnStatus {
        final byte status;
        final long txnId;

        TxnStatus(byte status, long txnId) {
            this.status = status;
            this.txnId = txnId;
        }
    }

    private static final class CommandComplete {
        final long rows;
        final long lastId;
        final String tag;

        CommandComplete(long rows, long lastId, String tag) {
            this.rows = rows;
            this.lastId = lastId;
            this.tag = tag;
        }
    }

    private static final class ProtocolError {
        final String severity;
        final String sqlState;
        final String message;
        final String detail;
        final String hint;

        ProtocolError(String severity, String sqlState, String message, String detail, String hint) {
            this.severity = severity;
            this.sqlState = sqlState;
            this.message = message;
            this.detail = detail;
            this.hint = hint;
        }
    }
}
