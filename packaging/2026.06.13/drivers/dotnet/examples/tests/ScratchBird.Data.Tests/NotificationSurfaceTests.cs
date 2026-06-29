// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System;
using System.Collections;
using System.Data;
using System.IO;
using System.Reflection;
using ScratchBird.Data;
using Xunit;

namespace ScratchBird.Data.Tests;

public class NotificationSurfaceTests
{
    [Fact]
    public void AddNotificationListener_DeduplicatesListenerAndInstallsBridgeOnce()
    {
        using var connection = CreateOpenConnectionWithHealthyClient(out var client);
        var callbacks = 0;
        Action<ScratchBirdNotification> listener = _ => callbacks++;

        connection.AddNotificationListener(listener);
        connection.AddNotificationListener(listener);
        connection.AcceptNotification(7, "channel.events", new byte[] { 1, 2 }, 'I', 42);

        Assert.Equal(1, callbacks);
        Assert.Equal(1, GetNotificationBridgeHandlerCount(client));
    }

    [Fact]
    public void NotificationQueueAndListenerPayloadsAreIsolationSafe()
    {
        using var connection = CreateOpenConnectionWithHealthyClient(out _);
        byte[]? listenerPayload = null;
        connection.AddNotificationListener(notification =>
        {
            listenerPayload = notification.Payload;
            notification.Payload[0] = 77;
        });

        connection.AcceptNotification(9, "channel.audit", new byte[] { 3, 4 }, 'U', 24);
        var queued = connection.GetNotification();

        Assert.NotNull(queued);
        Assert.NotNull(listenerPayload);
        Assert.Equal(77, listenerPayload![0]);
        Assert.Equal(3, queued!.Payload[0]);
        Assert.Equal("channel.audit", queued.Channel);
        Assert.Equal((ulong?)24, queued.RowId);
    }

    [Fact]
    public void GetNotifications_DrainsQueueAndClearRemovesPendingItems()
    {
        using var connection = CreateOpenConnectionWithHealthyClient(out _);
        connection.AcceptNotification(1, "c1", new byte[] { 1 }, null, null);
        connection.AcceptNotification(2, "c2", new byte[] { 2 }, 'D', 8);

        var drained = connection.GetNotifications();
        Assert.Equal(2, drained.Count);
        Assert.Null(connection.GetNotification());

        connection.AcceptNotification(3, "c3", new byte[] { 3 }, 'I', 9);
        connection.ClearNotifications();
        Assert.Null(connection.GetNotification());
    }

    [Fact]
    public void RemoveNotificationListener_StopsCallbacks()
    {
        using var connection = CreateOpenConnectionWithHealthyClient(out _);
        var callbacks = 0;
        Action<ScratchBirdNotification> listener = _ => callbacks++;

        connection.AddNotificationListener(listener);
        Assert.True(connection.RemoveNotificationListener(listener));
        Assert.False(connection.RemoveNotificationListener(listener));

        connection.AcceptNotification(4, "channel", new byte[] { 4 }, null, null);
        Assert.Equal(0, callbacks);
    }

    [Fact]
    public void NotificationApi_ThrowsWhenConnectionIsClosed()
    {
        using var connection = new ScratchBirdConnection(
            "Host=diag.local;Port=13092;Database=diagdb;Username=app;Password=secret;Pooling=false");

        Assert.Throws<InvalidOperationException>(() => connection.AddNotificationListener(_ => { }));
        Assert.Throws<InvalidOperationException>(() => connection.GetNotification());
        Assert.Throws<InvalidOperationException>(() => connection.GetNotifications());
        Assert.Throws<InvalidOperationException>(() => connection.ClearNotifications());
        Assert.Throws<InvalidOperationException>(() => connection.RemoveNotificationListener(_ => { }));
    }

    private static ScratchBirdConnection CreateOpenConnectionWithHealthyClient(out ProtocolClient client)
    {
        var connection = new ScratchBirdConnection(
            "Host=diag.local;Port=13092;Database=diagdb;Username=app;Password=secret;Pooling=false");
        client = new ProtocolClient();
        SetPrivateField(client, "_connected", true);
        SetPrivateField(client, "_stream", new MemoryStream());
        SetPrivateField(connection, "_client", client);
        SetPrivateField(connection, "_state", ConnectionState.Open);
        return connection;
    }

    private static int GetNotificationBridgeHandlerCount(ProtocolClient client)
    {
        var field = typeof(ProtocolClient).GetField("_notificationHandlers", BindingFlags.Instance | BindingFlags.NonPublic);
        Assert.NotNull(field);
        var handlers = field!.GetValue(client) as ICollection;
        Assert.NotNull(handlers);
        return handlers!.Count;
    }

    private static void SetPrivateField(object target, string fieldName, object value)
    {
        var field = target.GetType().GetField(fieldName, BindingFlags.Instance | BindingFlags.NonPublic);
        Assert.NotNull(field);
        field!.SetValue(target, value);
    }
}
