// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.sql.SQLFeatureNotSupportedException;
import java.sql.SQLException;
import java.util.Arrays;
import java.util.Properties;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import org.junit.jupiter.api.Test;

class SBAuthBootstrapContractTest {
    private static final int PROTOCOL_MAGIC = 0x50574253;
    private static final int PROTOCOL_VERSION_MAJOR = 1;
    private static final int PROTOCOL_VERSION_MINOR = 1;
    private static final int HEADER_SIZE = 40;
    private static final int MANAGER_PROTOCOL_MAGIC = 0x42444253;
    private static final int MANAGER_PROTOCOL_VERSION = 0x0101;
    private static final int MANAGER_HEADER_SIZE = 12;

    private static final byte MSG_STARTUP = 0x01;
    private static final byte MSG_AUTH_RESPONSE = 0x02;
    private static final byte MSG_AUTH_REQUEST = 0x40;
    private static final byte MSG_AUTH_OK = 0x41;
    private static final byte MSG_AUTH_CONTINUE = 0x42;
    private static final byte MSG_READY = 0x43;

    private static final byte MCP_MSG_CONNECT_RESPONSE = 0x02;
    private static final byte MCP_MSG_AUTH_CHALLENGE = 0x12;
    private static final byte MCP_MSG_AUTH_RESPONSE = 0x11;
    private static final byte MCP_MSG_STATUS_RESPONSE = 0x64;
    private static final byte MCP_MSG_HELLO = 0x65;
    private static final byte MCP_MSG_AUTH_START = 0x66;
    private static final byte MCP_MSG_DB_CONNECT = 0x69;
    private static final byte MCP_AUTH_METHOD_TOKEN = 4;

    private static final int AUTH_PASSWORD = 1;
    private static final int AUTH_SCRAM_SHA_512 = 4;
    private static final int AUTH_TOKEN = 5;
    private static final int AUTH_PEER = 6;

    @Test
    void probeAuthSurfaceDirectReportsScramSha512() throws Exception {
        try (TestServer server = startServer(socket -> {
            ProtocolFrame startup = readProtocolFrame(socket.getInputStream());
            assertEquals(MSG_STARTUP, startup.type);
            writeProtocolFrame(socket.getOutputStream(), MSG_AUTH_REQUEST, authRequestPayload(AUTH_SCRAM_SHA_512));
        })) {
            Properties info = new Properties();
            info.setProperty("user", "alice");
            SBAuthProbeResult result = SBDriver.probeAuthSurface(
                "jdbc:scratchbird://127.0.0.1:" + server.port() + "/db1?sslmode=disable",
                info);

            assertTrue(result.isReachable());
            assertEquals("direct", result.getFrontDoorMode());
            assertEquals("127.0.0.1", result.getResolvedHost());
            assertEquals(server.port(), result.getResolvedPort());
            assertEquals(AUTH_SCRAM_SHA_512, result.getRequiredMethodCode());
            assertEquals("SCRAM_SHA_512", result.getRequiredMethodName());
            assertEquals("scratchbird.auth.scram_sha_512", result.getRequiredAuthPluginId());
            assertTrue(result.isAdditionalContinuationPossible());
            assertEquals(1, result.getAdmittedMethods().size());
            assertEquals("SCRAM_SHA_512", result.getAdmittedMethods().get(0).getMethodName());
            assertTrue(result.getAdmittedMethods().get(0).isExecutableLocally());
            server.await();
        }
    }

    @Test
    void probeAuthSurfaceManagerProxyReportsToken() throws Exception {
        try (TestServer server = startServer(socket -> {
            ManagerFrame hello = readManagerFrame(socket.getInputStream());
            assertEquals(MCP_MSG_HELLO, hello.type);
            writeManagerFrame(socket.getOutputStream(), MCP_MSG_STATUS_RESPONSE, new byte[0]);

            ManagerFrame authStart = readManagerFrame(socket.getInputStream());
            assertEquals(MCP_MSG_AUTH_START, authStart.type);
            ByteBuffer payload = ByteBuffer.wrap(authStart.payload).order(ByteOrder.LITTLE_ENDIAN);
            int userLen = payload.getInt();
            byte[] userBytes = new byte[userLen];
            payload.get(userBytes);
            assertEquals("alice", new String(userBytes, StandardCharsets.UTF_8));
            assertEquals(MCP_AUTH_METHOD_TOKEN, payload.get() & 0xff);
            assertEquals(0, payload.getInt());

            writeManagerFrame(socket.getOutputStream(), MCP_MSG_AUTH_CHALLENGE, new byte[0]);
        })) {
            SBConnectionProperties props = new SBConnectionProperties();
            props.setHost("127.0.0.1");
            props.setPort(server.port());
            props.setDatabase("db1");
            props.setUser("alice");
            props.setSslMode("disable");
            props.setFrontDoorMode("manager_proxy");

            SBAuthProbeResult result = SBConnection.probeAuthSurface(props);

            assertTrue(result.isReachable());
            assertEquals("manager_proxy", result.getFrontDoorMode());
            assertEquals(AUTH_TOKEN, result.getRequiredMethodCode());
            assertEquals("TOKEN", result.getRequiredMethodName());
            assertEquals("scratchbird.auth.authkey_token", result.getRequiredAuthPluginId());
            assertTrue(result.isAdditionalContinuationPossible());
            assertEquals(1, result.getAdmittedMethods().size());
            assertEquals("TOKEN", result.getAdmittedMethods().get(0).getMethodName());
            server.await();
        }
    }

    @Test
    void connectionResolvedAuthContextReportsScramSha512() throws Exception {
        try (TestServer server = startServer(socket -> {
            InputStream in = socket.getInputStream();
            OutputStream out = socket.getOutputStream();

            ProtocolFrame startup = readProtocolFrame(in);
            assertEquals(MSG_STARTUP, startup.type);
            writeProtocolFrame(out, MSG_AUTH_REQUEST, authRequestPayload(AUTH_SCRAM_SHA_512));

            ProtocolFrame clientFirstFrame = readProtocolFrame(in);
            assertEquals(MSG_AUTH_RESPONSE, clientFirstFrame.type);
            String clientFirst = new String(clientFirstFrame.payload, StandardCharsets.UTF_8);
            String clientNonce = extractClientNonce(clientFirst);

            SBScramClient oracle = new SBScramClient("alice", SBScramClient.Algorithm.SHA_512, clientNonce);
            oracle.getClientFirstMessage();
            String serverFirst =
                "r=" + clientNonce + "-server,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";
            String expectedClientFinal = oracle.handleServerFirst(serverFirst, "secret");

            writeProtocolFrame(out, MSG_AUTH_CONTINUE, authContinuePayload(AUTH_SCRAM_SHA_512, serverFirst));

            ProtocolFrame clientFinalFrame = readProtocolFrame(in);
            assertEquals(MSG_AUTH_RESPONSE, clientFinalFrame.type);
            assertEquals(expectedClientFinal, new String(clientFinalFrame.payload, StandardCharsets.UTF_8));

            writeProtocolFrame(out, MSG_AUTH_OK, authOkPayload("v=" + oracle.getServerSignatureBase64()));
            writeProtocolFrame(out, MSG_READY, readyPayload((byte) 0, 0));
        })) {
            SBConnectionProperties props = new SBConnectionProperties();
            props.setHost("127.0.0.1");
            props.setPort(server.port());
            props.setDatabase("db1");
            props.setUser("alice");
            props.setPassword("secret");
            props.setSslMode("disable");

            SBProtocolHandler protocol = new SBProtocolHandler(props);
            protocol.connect();
            SBResolvedAuthContext context = protocol.getResolvedAuthContext();
            assertEquals("direct", context.getFrontDoorMode());
            assertTrue(context.isAttached());
            assertEquals(AUTH_SCRAM_SHA_512, context.getResolvedMethodCode());
            assertEquals("SCRAM_SHA_512", context.getResolvedMethodName());
            assertEquals("scratchbird.auth.scram_sha_512", context.getResolvedAuthPluginId());
            protocol.close();
            server.await();
        }
    }

    @Test
    void connectionResolvedAuthContextReportsTokenExecution() throws Exception {
        try (TestServer server = startServer(socket -> {
            InputStream in = socket.getInputStream();
            OutputStream out = socket.getOutputStream();

            ProtocolFrame startup = readProtocolFrame(in);
            assertEquals(MSG_STARTUP, startup.type);
            writeProtocolFrame(out, MSG_AUTH_REQUEST, authRequestPayload(AUTH_TOKEN));

            ProtocolFrame tokenFrame = readProtocolFrame(in);
            assertEquals(MSG_AUTH_RESPONSE, tokenFrame.type);
            assertEquals("bearer-token", new String(tokenFrame.payload, StandardCharsets.UTF_8));

            writeProtocolFrame(out, MSG_AUTH_OK, authOkPayload(""));
            writeProtocolFrame(out, MSG_READY, readyPayload((byte) 0, 0));
        })) {
            SBConnectionProperties props = new SBConnectionProperties();
            props.setHost("127.0.0.1");
            props.setPort(server.port());
            props.setDatabase("db1");
            props.setUser("alice");
            props.setAuthToken("bearer-token");
            props.setSslMode("disable");

            SBProtocolHandler protocol = new SBProtocolHandler(props);
            protocol.connect();
            SBResolvedAuthContext context = protocol.getResolvedAuthContext();
            assertTrue(context.isAttached());
            assertEquals("TOKEN", context.getResolvedMethodName());
            assertEquals("scratchbird.auth.authkey_token", context.getResolvedAuthPluginId());
            protocol.close();
            server.await();
        }
    }

    @Test
    void peerAuthFailsClosedButResolvedContextStillReportsPeer() throws Exception {
        try (TestServer server = startServer(socket -> {
            ProtocolFrame startup = readProtocolFrame(socket.getInputStream());
            assertEquals(MSG_STARTUP, startup.type);
            writeProtocolFrame(socket.getOutputStream(), MSG_AUTH_REQUEST, authRequestPayload(AUTH_PEER));
        })) {
            SBConnectionProperties props = new SBConnectionProperties();
            props.setHost("127.0.0.1");
            props.setPort(server.port());
            props.setDatabase("db1");
            props.setUser("alice");
            props.setSslMode("disable");

            SBProtocolHandler protocol = new SBProtocolHandler(props);
            SQLFeatureNotSupportedException ex =
                assertThrows(SQLFeatureNotSupportedException.class, protocol::connect);
            assertEquals("0A000", ex.getSQLState());
            SBResolvedAuthContext context = protocol.getResolvedAuthContext();
            assertEquals("PEER", context.getResolvedMethodName());
            assertEquals("scratchbird.auth.peer_uid", context.getResolvedAuthPluginId());
            assertFalse(context.isAttached());
            server.await();
        }
    }

    @Test
    void managerProxyConnectReportsManagerAuthenticatedContext() throws Exception {
        try (TestServer server = startServer(socket -> {
            InputStream in = socket.getInputStream();
            OutputStream out = socket.getOutputStream();

            ManagerFrame hello = readManagerFrame(in);
            assertEquals(MCP_MSG_HELLO, hello.type);
            writeManagerFrame(out, MCP_MSG_STATUS_RESPONSE, new byte[0]);

            ManagerFrame authStart = readManagerFrame(in);
            assertEquals(MCP_MSG_AUTH_START, authStart.type);
            ByteBuffer authStartPayload = ByteBuffer.wrap(authStart.payload).order(ByteOrder.LITTLE_ENDIAN);
            int userLen = authStartPayload.getInt();
            byte[] userBytes = new byte[userLen];
            authStartPayload.get(userBytes);
            assertEquals("alice", new String(userBytes, StandardCharsets.UTF_8));
            assertEquals(MCP_AUTH_METHOD_TOKEN, authStartPayload.get() & 0xff);
            int tokenLen = authStartPayload.getInt();
            byte[] managerToken = new byte[tokenLen];
            authStartPayload.get(managerToken);
            assertEquals("manager-token", new String(managerToken, StandardCharsets.UTF_8));

            writeManagerFrame(out, MCP_MSG_AUTH_RESPONSE, managerAuthResponsePayload(true));

            ManagerFrame dbConnect = readManagerFrame(in);
            assertEquals(MCP_MSG_DB_CONNECT, dbConnect.type);
            writeManagerFrame(out, MCP_MSG_CONNECT_RESPONSE, managerConnectResponsePayload(true));

            ProtocolFrame startup = readProtocolFrame(in);
            assertEquals(MSG_STARTUP, startup.type);
            writeProtocolFrame(out, MSG_AUTH_REQUEST, authRequestPayload(AUTH_PASSWORD));

            ProtocolFrame passwordFrame = readProtocolFrame(in);
            assertEquals(MSG_AUTH_RESPONSE, passwordFrame.type);
            assertEquals("secret", new String(passwordFrame.payload, StandardCharsets.UTF_8));

            writeProtocolFrame(out, MSG_AUTH_OK, authOkPayload(""));
            writeProtocolFrame(out, MSG_READY, readyPayload((byte) 0, 0));
        })) {
            SBConnectionProperties props = new SBConnectionProperties();
            props.setHost("127.0.0.1");
            props.setPort(server.port());
            props.setDatabase("db1");
            props.setUser("alice");
            props.setPassword("secret");
            props.setSslMode("disable");
            props.setFrontDoorMode("manager_proxy");
            props.setManagerAuthToken("manager-token");

            SBProtocolHandler protocol = new SBProtocolHandler(props);
            protocol.connect();
            SBResolvedAuthContext context = protocol.getResolvedAuthContext();
            assertEquals("manager_proxy", context.getFrontDoorMode());
            assertTrue(context.isManagerAuthenticated());
            assertTrue(context.isAttached());
            assertEquals("PASSWORD", context.getResolvedMethodName());
            protocol.close();
            server.await();
        }
    }

    private static String extractClientNonce(String clientFirst) {
        for (String part : clientFirst.split(",")) {
            if (part.startsWith("r=")) {
                return part.substring(2);
            }
        }
        throw new IllegalStateException("client nonce missing");
    }

    private static byte[] authRequestPayload(int method) {
        byte[] payload = new byte[4];
        payload[0] = (byte) method;
        return payload;
    }

    private static byte[] authContinuePayload(int method, String data) {
        byte[] dataBytes = data.getBytes(StandardCharsets.UTF_8);
        ByteBuffer buf = ByteBuffer.allocate(8 + dataBytes.length).order(ByteOrder.LITTLE_ENDIAN);
        buf.put((byte) method);
        buf.put((byte) 1);
        buf.putShort((short) 0);
        buf.putInt(dataBytes.length);
        buf.put(dataBytes);
        return buf.array();
    }

    private static byte[] authOkPayload(String serverInfo) {
        byte[] infoBytes = serverInfo == null ? new byte[0] : serverInfo.getBytes(StandardCharsets.UTF_8);
        ByteBuffer buf = ByteBuffer.allocate(20 + infoBytes.length).order(ByteOrder.LITTLE_ENDIAN);
        buf.put(new byte[16]);
        buf.putInt(infoBytes.length);
        buf.put(infoBytes);
        return buf.array();
    }

    private static byte[] readyPayload(byte status, long txnId) {
        ByteBuffer buf = ByteBuffer.allocate(20).order(ByteOrder.LITTLE_ENDIAN);
        buf.put(status);
        buf.put(new byte[3]);
        buf.putLong(txnId);
        buf.putLong(0L);
        return buf.array();
    }

    private static byte[] managerAuthResponsePayload(boolean success) {
        byte[] payload = new byte[1 + 4 + 256];
        payload[0] = success ? (byte) 0 : (byte) 1;
        return payload;
    }

    private static byte[] managerConnectResponsePayload(boolean success) {
        byte[] payload = new byte[1 + 2 + 2 + 16 + 64 + 32];
        payload[0] = success ? (byte) 0 : (byte) 1;
        return payload;
    }

    private static TestServer startServer(ServerHandler handler) throws IOException {
        return new TestServer(handler);
    }

    @FunctionalInterface
    private interface ServerHandler {
        void handle(Socket socket) throws Exception;
    }

    private static final class TestServer implements AutoCloseable {
        private final ServerSocket serverSocket;
        private final ExecutorService executor;
        private final Future<?> future;

        private TestServer(ServerHandler handler) throws IOException {
            serverSocket = new ServerSocket(0, 1, InetAddress.getLoopbackAddress());
            executor = Executors.newSingleThreadExecutor();
            future = executor.submit(() -> {
                try (Socket socket = serverSocket.accept()) {
                    socket.setSoTimeout(5000);
                    handler.handle(socket);
                }
                return null;
            });
        }

        private int port() {
            return serverSocket.getLocalPort();
        }

        private void await() throws InterruptedException, ExecutionException, TimeoutException {
            future.get(5, TimeUnit.SECONDS);
        }

        @Override
        public void close() {
            try {
                serverSocket.close();
            } catch (IOException ignore) {
                // ignore
            }
            executor.shutdownNow();
            try {
                future.get(5, TimeUnit.SECONDS);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            } catch (ExecutionException | TimeoutException ignore) {
                // surfaced by explicit await() in successful tests
            }
        }
    }

    private record ProtocolFrame(byte type, byte[] payload) {
    }

    private record ManagerFrame(byte type, byte[] payload) {
    }

    private static ProtocolFrame readProtocolFrame(InputStream in) throws IOException {
        byte[] header = readFully(in, HEADER_SIZE);
        ByteBuffer buf = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        int magic = buf.getInt();
        assertEquals(PROTOCOL_MAGIC, magic);
        assertEquals(PROTOCOL_VERSION_MAJOR, buf.get() & 0xff);
        assertEquals(PROTOCOL_VERSION_MINOR, buf.get() & 0xff);
        byte type = buf.get();
        buf.get(); // flags
        int length = buf.getInt();
        buf.getInt(); // sequence
        buf.position(buf.position() + 16 + 8);
        return new ProtocolFrame(type, readFully(in, length));
    }

    private static void writeProtocolFrame(OutputStream out, byte type, byte[] payload) throws IOException {
        ByteBuffer buf = ByteBuffer.allocate(HEADER_SIZE + payload.length).order(ByteOrder.LITTLE_ENDIAN);
        buf.putInt(PROTOCOL_MAGIC);
        buf.put((byte) PROTOCOL_VERSION_MAJOR);
        buf.put((byte) PROTOCOL_VERSION_MINOR);
        buf.put(type);
        buf.put((byte) 0);
        buf.putInt(payload.length);
        buf.putInt(0);
        buf.put(new byte[16]);
        buf.putLong(0L);
        buf.put(payload);
        out.write(buf.array());
        out.flush();
    }

    private static ManagerFrame readManagerFrame(InputStream in) throws IOException {
        byte[] header = readFully(in, MANAGER_HEADER_SIZE);
        ByteBuffer buf = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        assertEquals(MANAGER_PROTOCOL_MAGIC, buf.getInt());
        assertEquals(MANAGER_PROTOCOL_VERSION, Short.toUnsignedInt(buf.getShort()));
        byte type = buf.get();
        buf.get(); // flags
        int length = buf.getInt();
        return new ManagerFrame(type, readFully(in, length));
    }

    private static void writeManagerFrame(OutputStream out, byte type, byte[] payload) throws IOException {
        ByteBuffer buf = ByteBuffer.allocate(MANAGER_HEADER_SIZE + payload.length).order(ByteOrder.LITTLE_ENDIAN);
        buf.putInt(MANAGER_PROTOCOL_MAGIC);
        buf.putShort((short) MANAGER_PROTOCOL_VERSION);
        buf.put(type);
        buf.put((byte) 0);
        buf.putInt(payload.length);
        buf.put(payload);
        out.write(buf.array());
        out.flush();
    }

    private static byte[] readFully(InputStream in, int length) throws IOException {
        byte[] buffer = new byte[length];
        int offset = 0;
        while (offset < length) {
            int read = in.read(buffer, offset, length - offset);
            if (read < 0) {
                throw new EOFException("unexpected end of stream");
            }
            offset += read;
        }
        return buffer;
    }
}
