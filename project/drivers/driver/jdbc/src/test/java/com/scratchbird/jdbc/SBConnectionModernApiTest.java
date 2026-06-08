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
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Field;
import java.nio.charset.StandardCharsets;
import java.sql.JDBCType;
import java.sql.ShardingKey;
import java.sql.ShardingKeyBuilder;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import sun.misc.Unsafe;

class SBConnectionModernApiTest {

    @Test
    void supportsRequestScopeAndShardingKeySetters() throws Exception {
        SBConnection connection = newConnectionForTest();
        ShardingKey shard = new ShardingKey() {};
        ShardingKey superShard = new ShardingKey() {};

        connection.beginRequest();
        assertTrue((Boolean) getField(connection, "requestScopeActive"));
        connection.endRequest();
        assertFalse((Boolean) getField(connection, "requestScopeActive"));

        assertTrue(connection.setShardingKeyIfValid(shard, 1));
        assertEquals(shard, getField(connection, "shardingKey"));
        assertNull(getField(connection, "superShardingKey"));

        assertTrue(connection.setShardingKeyIfValid(shard, superShard, 0));
        assertEquals(shard, getField(connection, "shardingKey"));
        assertEquals(superShard, getField(connection, "superShardingKey"));

        assertFalse(connection.setShardingKeyIfValid(null, 2));
        assertFalse(connection.setShardingKeyIfValid(null, superShard, 2));

        connection.setShardingKey(shard);
        assertEquals(shard, getField(connection, "shardingKey"));
        assertNull(getField(connection, "superShardingKey"));

        connection.setShardingKey(shard, superShard);
        assertEquals(shard, getField(connection, "shardingKey"));
        assertEquals(superShard, getField(connection, "superShardingKey"));
    }

    @Test
    void supportsShardingKeyBuilderAndBuiltKeys() throws Exception {
        SBConnection connection = newConnectionForTest();

        ShardingKeyBuilder builder = connection.createShardingKeyBuilder();
        ShardingKey shard = builder
            .subkey("tenant-acme", JDBCType.VARCHAR)
            .subkey(42, JDBCType.INTEGER)
            .build();
        assertTrue(connection.setShardingKeyIfValid(shard, 0));
        assertEquals(shard, getField(connection, "shardingKey"));

        ShardingKey superShard = connection.createShardingKeyBuilder()
            .subkey("region-us", JDBCType.VARCHAR)
            .build();
        assertTrue(connection.setShardingKeyIfValid(shard, superShard, 0));
        assertEquals(shard, getField(connection, "shardingKey"));
        assertEquals(superShard, getField(connection, "superShardingKey"));
    }

    @Test
    void shardingKeyBuilderRejectsNullSqlType() throws Exception {
        SBConnection connection = newConnectionForTest();
        ShardingKeyBuilder builder = connection.createShardingKeyBuilder();
        IllegalArgumentException ex = assertThrows(IllegalArgumentException.class,
            () -> builder.subkey("tenant", (java.sql.SQLType) null));
        assertTrue(ex.getMessage().toLowerCase().contains("sqltype"));
    }

    @Test
    void rejectsInvalidShardingArguments() throws Exception {
        SBConnection connection = newConnectionForTest();
        ShardingKey shard = new ShardingKey() {};

        SQLException timeoutSingle = assertThrows(SQLException.class,
            () -> connection.setShardingKeyIfValid(shard, -1));
        assertEquals("HY024", timeoutSingle.getSQLState());

        SQLException timeoutComposite = assertThrows(SQLException.class,
            () -> connection.setShardingKeyIfValid(shard, new ShardingKey() {}, -1));
        assertEquals("HY024", timeoutComposite.getSQLState());

        SQLException nullSingle = assertThrows(SQLException.class,
            () -> connection.setShardingKey((ShardingKey) null));
        assertEquals("HY024", nullSingle.getSQLState());

        SQLException nullComposite = assertThrows(SQLException.class,
            () -> connection.setShardingKey((ShardingKey) null, new ShardingKey() {}));
        assertEquals("HY024", nullComposite.getSQLState());
    }

    @Test
    void networkTimeoutRequiresExecutorAndSupportsRoundTrip() throws Exception {
        TimeoutProtocol protocol = new TimeoutProtocol();
        SBConnection connection = newConnectionForTest(protocol);

        SQLException nullExecutor = assertThrows(SQLException.class,
            () -> connection.setNetworkTimeout(null, 100));
        assertEquals("HY000", nullExecutor.getSQLState());

        SQLException negative = assertThrows(SQLException.class,
            () -> connection.setNetworkTimeout(Runnable::run, -1));
        assertEquals("HY024", negative.getSQLState());

        connection.setNetworkTimeout(Runnable::run, 2500);
        assertEquals(2500, connection.getNetworkTimeout());
    }

    @Test
    void abortUsesExecutorAndTransitionsConnectionClosedState() throws Exception {
        TimeoutProtocol protocol = new TimeoutProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        AtomicInteger executed = new AtomicInteger();
        Executor direct = command -> {
            executed.incrementAndGet();
            command.run();
        };

        connection.abort(direct);

        assertEquals(1, executed.get());
        assertEquals(1, protocol.abortCalls.get());
        assertTrue(connection.isClosed());
    }

    @Test
    void notificationListenersAndQueueReceiveProtocolNotifications() throws Exception {
        NotificationProtocol protocol = new NotificationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        AtomicInteger callbacks = new AtomicInteger();

        connection.addNotificationListener(notification -> {
            callbacks.incrementAndGet();
            assertEquals("events.job", notification.getChannel());
            assertEquals("job-ready", notification.getPayloadText());
            assertEquals(Character.valueOf('U'), notification.getChangeType());
            assertEquals(Long.valueOf(77L), notification.getRowId());
        });

        protocol.emit("events.job", "job-ready", 'U', 77L);

        assertEquals(1, callbacks.get());
        SBConnection.Notification queued = connection.getNotification();
        assertNotNull(queued);
        assertEquals("events.job", queued.getChannel());
        assertEquals("job-ready", queued.getPayloadText());
        assertNull(connection.getNotification());
    }

    @Test
    void removingNotificationListenerStopsCallbacksButKeepsQueueing() throws Exception {
        NotificationProtocol protocol = new NotificationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        AtomicInteger callbacks = new AtomicInteger();
        SBConnection.NotificationListener listener = notice -> callbacks.incrementAndGet();

        connection.addNotificationListener(listener);
        assertTrue(connection.removeNotificationListener(listener));
        assertFalse(connection.removeNotificationListener(listener));

        protocol.emit("events.audit", "rotate", null, null);

        assertEquals(0, callbacks.get());
        List<SBConnection.Notification> notifications = connection.getNotifications();
        assertEquals(1, notifications.size());
        assertEquals("events.audit", notifications.get(0).getChannel());
        assertEquals("rotate", notifications.get(0).getPayloadText());
        assertTrue(connection.getNotifications().isEmpty());
    }

    @Test
    void notificationPollingInstallsBridgeWithoutExplicitListener() throws Exception {
        NotificationProtocol protocol = new NotificationProtocol();
        SBConnection connection = newConnectionForTest(protocol);

        assertTrue(connection.getNotifications().isEmpty());
        protocol.emit("events.node", "online", 'I', 12L);

        SBConnection.Notification notice = connection.getNotification();
        assertNotNull(notice);
        assertEquals("events.node", notice.getChannel());
        assertEquals("online", notice.getPayloadText());
        connection.clearNotifications();
        assertNull(connection.getNotification());
    }

    @Test
    void listenUnlistenNotifyIssueExpectedCommands() throws Exception {
        NotificationCommandProtocol protocol = new NotificationCommandProtocol();
        SBConnection connection = newConnectionForTest(protocol);

        connection.listen("events.job");
        connection.notifyChannel("events.job");
        connection.notifyChannel("events.job", "ready'1");
        connection.unlisten("events.job");
        connection.unlistenAll();

        assertEquals(List.of(
            "LISTEN \"events.job\"",
            "NOTIFY \"events.job\"",
            "NOTIFY \"events.job\", 'ready''1'",
            "UNLISTEN \"events.job\"",
            "UNLISTEN *"
        ), protocol.commands());
    }

    @Test
    void listenAndNotifyValidateChannelAndPayloadInputs() throws Exception {
        NotificationCommandProtocol protocol = new NotificationCommandProtocol();
        SBConnection connection = newConnectionForTest(protocol);

        SQLException emptyChannel = assertThrows(SQLException.class, () -> connection.listen("   "));
        assertEquals("HY024", emptyChannel.getSQLState());

        SQLException nullChannel = assertThrows(SQLException.class, () -> connection.notifyChannel(null, "x"));
        assertEquals("HY024", nullChannel.getSQLState());

        SQLException badPayload = assertThrows(SQLException.class,
            () -> connection.notifyChannel("events.job", "bad\0payload"));
        assertEquals("HY024", badPayload.getSQLState());
    }

    @Test
    void closeDetachesNotificationBridgeListenerFromProtocol() throws Exception {
        NotificationProtocol protocol = new NotificationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        setField(connection, "autoCommit", true);
        connection.addNotificationListener(notification -> {
            // no-op
        });

        connection.close();

        assertTrue(connection.isClosed());
        assertEquals(1, protocol.removeCalls.get());
        assertEquals(1, protocol.closeCalls.get());
        assertNull(protocol.listener);
    }

    private static SBConnection newConnectionForTest() throws Exception {
        SBConnection connection = (SBConnection) getUnsafe().allocateInstance(SBConnection.class);
        setField(connection, "properties", new SBConnectionProperties());
        setField(connection, "closed", new AtomicBoolean(false));
        setField(connection, "circuitBreaker", new CircuitBreaker());
        setField(connection, "telemetry", new TelemetryCollector());
        setField(connection, "keepaliveManager", new KeepaliveManager());
        setField(connection, "requestScopeActive", false);
        return connection;
    }

    private static SBConnection newConnectionForTest(SBProtocolHandler protocol) throws Exception {
        SBConnection connection = newConnectionForTest();
        setField(connection, "protocol", protocol);
        return connection;
    }

    private static Unsafe getUnsafe() throws Exception {
        Field field = Unsafe.class.getDeclaredField("theUnsafe");
        field.setAccessible(true);
        return (Unsafe) field.get(null);
    }

    private static void setField(Object object, String fieldName, Object value) throws Exception {
        Field field = SBConnection.class.getDeclaredField(fieldName);
        field.setAccessible(true);
        field.set(object, value);
    }

    private static Object getField(Object object, String fieldName) throws Exception {
        Field field = SBConnection.class.getDeclaredField(fieldName);
        field.setAccessible(true);
        return field.get(object);
    }

    private static final class TimeoutProtocol extends SBProtocolHandler {
        private volatile int timeout;
        private final AtomicInteger abortCalls = new AtomicInteger();

        TimeoutProtocol() {
            super(new SBConnectionProperties());
        }

        @Override
        public synchronized void setNetworkTimeout(int milliseconds) {
            this.timeout = milliseconds;
        }

        @Override
        public synchronized int getNetworkTimeout() {
            return timeout;
        }

        @Override
        public synchronized void abort() {
            abortCalls.incrementAndGet();
        }
    }

    private static final class NotificationProtocol extends SBProtocolHandler {
        private volatile NotificationListener listener;
        private final AtomicInteger removeCalls = new AtomicInteger();
        private final AtomicInteger closeCalls = new AtomicInteger();

        NotificationProtocol() {
            super(new SBConnectionProperties());
        }

        @Override
        public void addNotificationListener(NotificationListener listener) {
            this.listener = listener;
        }

        @Override
        public void removeNotificationListener(NotificationListener listener) {
            removeCalls.incrementAndGet();
            if (this.listener == listener) {
                this.listener = null;
            }
        }

        @Override
        public synchronized void close() {
            closeCalls.incrementAndGet();
            listener = null;
        }

        void emit(String channel, String payload, Character changeType, Long rowId) {
            NotificationListener current = listener;
            if (current != null) {
                byte[] bytes = payload == null ? new byte[0] : payload.getBytes(StandardCharsets.UTF_8);
                current.onNotification(new NotificationMessage(1234, channel, bytes, changeType, rowId));
            }
        }
    }

    private static final class NotificationCommandProtocol extends SBProtocolHandler {
        private final List<String> commands = new ArrayList<>();
        private volatile NotificationListener listener;

        NotificationCommandProtocol() {
            super(new SBConnectionProperties());
        }

        @Override
        public synchronized SBQueryResult execute(String sql) {
            commands.add(sql);
            SBQueryResult result = new SBQueryResult();
            result.setUpdateCount(1);
            return result;
        }

        @Override
        public void addNotificationListener(NotificationListener listener) {
            this.listener = listener;
        }

        @Override
        public void removeNotificationListener(NotificationListener listener) {
            if (this.listener == listener) {
                this.listener = null;
            }
        }

        List<String> commands() {
            return List.copyOf(commands);
        }
    }
}
