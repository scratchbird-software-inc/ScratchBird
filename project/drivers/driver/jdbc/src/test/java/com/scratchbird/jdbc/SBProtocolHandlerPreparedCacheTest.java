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

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.OutputStream;
import java.lang.reflect.Field;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.List;
import org.junit.jupiter.api.Test;

public class SBProtocolHandlerPreparedCacheTest {

    private static final int PROTOCOL_MAGIC = 0x50574253;
    private static final int HEADER_SIZE = 40;
    private static final int PROTOCOL_VERSION_MAJOR = 1;
    private static final int PROTOCOL_VERSION_MINOR = 1;

    private static final byte MSG_PARSE = 0x04;
    private static final byte MSG_BIND = 0x05;
    private static final byte MSG_DESCRIBE = 0x06;
    private static final byte MSG_EXECUTE = 0x07;
    private static final byte MSG_SYNC = 0x09;
    private static final byte MSG_PARAMETER_DESCRIPTION = 0x50;
    private static final byte MSG_COMMAND_COMPLETE = 0x46;
    private static final byte MSG_READY = 0x43;
    private static final byte MSG_ERROR = 0x48;

    @Test
    public void preparedStatementCachesAfterFirstExecution() throws Exception {
        byte[] payloads = concatFrames(
            MSG_PARAMETER_DESCRIPTION, buildParameterDescriptionPayload(1),
            MSG_READY, buildReadyPayload(),
            MSG_COMMAND_COMPLETE, buildCommandCompletePayload(1),
            MSG_READY, buildReadyPayload(),
            MSG_COMMAND_COMPLETE, buildCommandCompletePayload(1),
            MSG_READY, buildReadyPayload()
        );
        assertValidProtocolFrames(payloads);

        var handler = handlerForResponsePayloads(payloads);
        var result1 = handler.execute("SELECT ?::INTEGER", List.of(11), List.of(), 0, 0);
        var result2 = handler.execute("SELECT ?::INTEGER", List.of(22), List.of(), 0, 0);
        assertEquals(1, result1.getUpdateCount());
        assertEquals(1, result2.getUpdateCount());

        byte[] sent = getSentBytes(handler);
        List<Byte> messageTypes = parseMessageTypes(sent);
        List<Byte> expected = List.of(
            MSG_PARSE, MSG_DESCRIBE, MSG_SYNC, MSG_BIND, MSG_EXECUTE, MSG_SYNC,
            MSG_BIND, MSG_EXECUTE, MSG_SYNC
        );
        assertEquals(expected.size(), messageTypes.size());
        assertEquals(expected, messageTypes);

        List<ByteArrayPayload> parsed = parseFramesWithPayload(sent);
        String preparedName = parseStatementNameFromParse(parsed.get(0).payload);
        String firstBind = parseStatementNameFromBind(parsed.get(3).payload);
        String secondBind = parseStatementNameFromBind(parsed.get(6).payload);
        assertEquals(preparedName, firstBind);
        assertEquals(preparedName, secondBind);
    }

    @Test
    public void recoverableCachedStatementErrorTriggersReprepare() throws Exception {
        byte[] payloads = concatFrames(
            MSG_PARAMETER_DESCRIPTION, buildParameterDescriptionPayload(1),
            MSG_READY, buildReadyPayload(),
            MSG_COMMAND_COMPLETE, buildCommandCompletePayload(1),
            MSG_READY, buildReadyPayload(),
            MSG_ERROR, buildErrorPayload("42P01", "prepared statement \"sb_stmt\" does not exist"),
            MSG_READY, buildReadyPayload(),
            MSG_PARAMETER_DESCRIPTION, buildParameterDescriptionPayload(1),
            MSG_READY, buildReadyPayload(),
            MSG_COMMAND_COMPLETE, buildCommandCompletePayload(1),
            MSG_READY, buildReadyPayload()
        );
        assertValidProtocolFrames(payloads);

        var handler = handlerForResponsePayloads(payloads);
        handler.execute("SELECT ?::INTEGER", List.of(11), List.of(), 0, 0);
        handler.execute("SELECT ?::INTEGER", List.of(22), List.of(), 0, 0);

        byte[] sent = getSentBytes(handler);
        List<Byte> messageTypes = parseMessageTypes(sent);
        List<Byte> parseMessages = messageTypes.stream()
            .filter(type -> type == MSG_PARSE)
            .toList();
        assertEquals(2, parseMessages.size());

        List<ByteArrayPayload> parsed = parseFramesWithPayload(sent);
        String firstParseName = parseStatementNameFromParse(parsed.get(0).payload);
        String retryParseName = parseStatementNameFromParse(parsed.get(6).payload);
        assertNotEquals(firstParseName, retryParseName);
    }

    @Test
    public void recoverableCachedStatementErrorTriggersReprepareWithPreparedStatementState() throws Exception {
        byte[] payloads = concatFrames(
            MSG_PARAMETER_DESCRIPTION, buildParameterDescriptionPayload(1),
            MSG_READY, buildReadyPayload(),
            MSG_COMMAND_COMPLETE, buildCommandCompletePayload(1),
            MSG_READY, buildReadyPayload(),
            MSG_ERROR, buildErrorPayload("26000", "Unknown prepared statement: sb_stmt"),
            MSG_READY, buildReadyPayload(),
            MSG_PARAMETER_DESCRIPTION, buildParameterDescriptionPayload(1),
            MSG_READY, buildReadyPayload(),
            MSG_COMMAND_COMPLETE, buildCommandCompletePayload(1),
            MSG_READY, buildReadyPayload()
        );
        assertValidProtocolFrames(payloads);

        var handler = handlerForResponsePayloads(payloads);
        handler.execute("SELECT ?::INTEGER", List.of(11), List.of(), 0, 0);
        handler.execute("SELECT ?::INTEGER", List.of(22), List.of(), 0, 0);

        byte[] sent = getSentBytes(handler);
        List<Byte> messageTypes = parseMessageTypes(sent);
        List<Byte> parseMessages = messageTypes.stream()
            .filter(type -> type == MSG_PARSE)
            .toList();
        assertEquals(2, parseMessages.size());
    }

    private static SBProtocolHandler handlerForResponsePayloads(byte[] payloads) throws Exception {
        SBProtocolHandler handler = new SBProtocolHandler(new SBConnectionProperties());
        setField(handler, "inputStream", new ByteArrayInputStream(payloads));
        setField(handler, "outputStream", new CaptureOutputStream());
        return handler;
    }

    private static void setField(SBProtocolHandler handler, String fieldName, Object value) throws Exception {
        Field field = SBProtocolHandler.class.getDeclaredField(fieldName);
        field.setAccessible(true);
        field.set(handler, value);
    }

    private static void assertValidProtocolFrames(byte[] raw) {
        List<Byte> seen = parseMessageTypes(raw);
        assertTrue(raw.length > 0);
        assertTrue(seen.size() > 0);
    }

    private static byte[] getSentBytes(SBProtocolHandler handler) throws Exception {
        Field outputStream = SBProtocolHandler.class.getDeclaredField("outputStream");
        outputStream.setAccessible(true);
        return ((CaptureOutputStream) outputStream.get(handler)).toByteArray();
    }

    private static byte[] buildParameterDescriptionPayload(int paramCount) {
        ByteBuffer payload = ByteBuffer.allocate(4 + (paramCount * 4));
        payload.order(ByteOrder.LITTLE_ENDIAN);
        payload.putShort((short) paramCount);
        payload.putShort((short) 0);
        for (int i = 0; i < paramCount; i++) {
            payload.putInt(23);
        }
        return payload.array();
    }

    private static byte[] buildCommandCompletePayload(long rows) {
        byte[] tag = "SELECT".getBytes(StandardCharsets.UTF_8);
        ByteBuffer payload = ByteBuffer.allocate(1 + 3 + 8 + 8 + tag.length);
        payload.order(ByteOrder.LITTLE_ENDIAN);
        payload.put((byte) 0);
        payload.put(new byte[3]);
        payload.putLong(rows);
        payload.putLong(0);
        payload.put(tag);
        return payload.array();
    }

    private static byte[] buildReadyPayload() {
        ByteBuffer payload = ByteBuffer.allocate(20);
        payload.order(ByteOrder.LITTLE_ENDIAN);
        payload.put((byte) 0);
        payload.put(new byte[3]);
        payload.putLong(0);
        payload.putLong(0);
        return payload.array();
    }

    private static byte[] buildErrorPayload(String state, String message) {
        byte[] stateBytes = state.getBytes(StandardCharsets.UTF_8);
        byte[] messageBytes = message.getBytes(StandardCharsets.UTF_8);
        ByteBuffer payload = ByteBuffer.allocate(
            1 + stateBytes.length + 1
            + 1 + messageBytes.length + 1
            + 1
        );
        payload.put((byte) 'C');
        payload.put(stateBytes);
        payload.put((byte) 0);
        payload.put((byte) 'M');
        payload.put(messageBytes);
        payload.put((byte) 0);
        payload.put((byte) 0);
        return payload.array();
    }

    private static byte[] buildFrame(byte type, byte[] payload) {
        ByteBuffer frame = ByteBuffer.allocate(HEADER_SIZE + payload.length);
        frame.order(ByteOrder.LITTLE_ENDIAN);
        frame.putInt(PROTOCOL_MAGIC);
        frame.put((byte) PROTOCOL_VERSION_MAJOR);
        frame.put((byte) PROTOCOL_VERSION_MINOR);
        frame.put(type);
        frame.put((byte) 0);
        frame.putInt(payload.length);
        frame.putInt(0);
        frame.put(new byte[16]);
        frame.putLong(0);
        frame.put(payload);
        return frame.array();
    }

    private static byte[] concatFrames(Object... fragments) {
        if (fragments.length % 2 != 0) {
            throw new IllegalArgumentException("concatFrames expects alternating type/payload args");
        }

        int totalLength = 0;
        int fragmentCount = fragments.length / 2;
        byte[][] frames = new byte[fragmentCount][];
        for (int i = 0; i < fragmentCount; i++) {
            int offset = i * 2;
            byte type = (byte) ((Number) fragments[offset]).intValue();
            byte[] payload = (byte[]) fragments[offset + 1];
            byte[] frame = buildFrame(type, payload);
            frames[i] = frame;
            totalLength += frame.length;
        }

        byte[] combined = new byte[totalLength];
        int offset = 0;
        for (byte[] frame : frames) {
            System.arraycopy(frame, 0, combined, offset, frame.length);
            offset += frame.length;
        }
        return combined;
    }

    private static List<Byte> parseMessageTypes(byte[] raw) {
        List<Byte> types = new ArrayList<>();
        ByteBuffer buf = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN);
        while (buf.remaining() >= HEADER_SIZE) {
            int magic = buf.getInt();
            assertEquals(PROTOCOL_MAGIC, magic);
            buf.get();
            buf.get();
            byte type = buf.get();
            types.add(type);
            buf.get();
            int len = buf.getInt();
            buf.getInt();
            if (buf.position() + 16 + 8 + len > raw.length) {
                break;
            }
            buf.position(buf.position() + 16 + 8 + len);
        }
        return types;
    }

    private static List<ByteArrayPayload> parseFramesWithPayload(byte[] raw) {
        List<ByteArrayPayload> payloads = new ArrayList<>();
        ByteBuffer buf = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN);
        while (buf.remaining() >= HEADER_SIZE) {
            int magic = buf.getInt();
            assertEquals(PROTOCOL_MAGIC, magic);
            byte major = buf.get();
            assertEquals(PROTOCOL_VERSION_MAJOR, major);
            byte minor = buf.get();
            assertEquals(PROTOCOL_VERSION_MINOR, minor);
            byte type = buf.get();
            buf.get();
            int len = buf.getInt();
            buf.getInt();
            buf.position(buf.position() + 16 + 8);
            if (buf.remaining() < len) {
                break;
            }
            byte[] payload = new byte[len];
            buf.get(payload);
            payloads.add(new ByteArrayPayload(type, payload));
        }
        return payloads;
    }

    private static String parseStatementNameFromParse(byte[] payload) {
        ByteBuffer payloadBuf = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        int nameLen = payloadBuf.getInt();
        byte[] nameBytes = new byte[nameLen];
        payloadBuf.get(nameBytes);
        return new String(nameBytes, StandardCharsets.UTF_8);
    }

    private static String parseStatementNameFromBind(byte[] payload) {
        ByteBuffer payloadBuf = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        int portalLen = payloadBuf.getInt();
        if (portalLen > 0) {
            payloadBuf.position(payloadBuf.position() + portalLen);
        }
        int statementNameLen = payloadBuf.getInt();
        byte[] statementName = new byte[statementNameLen];
        payloadBuf.get(statementName);
        return new String(statementName, StandardCharsets.UTF_8);
    }

    private static final class CaptureOutputStream extends OutputStream {
        private final ByteArrayOutputStream delegate = new ByteArrayOutputStream();
        private boolean flushed;

        @Override
        public void write(int b) {
            delegate.write(b);
        }

        @Override
        public void flush() {
            flushed = true;
        }

        public byte[] toByteArray() {
            assertTrue(flushed || delegate.size() > 0);
            return delegate.toByteArray();
        }
    }

    private static final class ByteArrayPayload {
        final byte type;
        final byte[] payload;

        ByteArrayPayload(byte type, byte[] payload) {
            this.type = type;
            this.payload = payload;
        }
    }
}
