// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System;
using System.Data;
using System.Reflection;
using ScratchBird.Data;
using Xunit;

namespace ScratchBird.Data.Tests;

public class DiagnosticsSurfaceTests
{
    [Fact]
    public void GetDiagnostics_WhenClosedExposesSafeConnectionState()
    {
        using var connection = new ScratchBirdConnection(
            "Host=diag.local;Port=13092;Database=diagdb;Username=app;Password=secret;Pooling=false");

        var diagnostics = connection.GetDiagnostics();

        Assert.Equal(ConnectionState.Closed, diagnostics.State);
        Assert.False(diagnostics.IsHealthy);
        Assert.Equal("diag.local", diagnostics.Host);
        Assert.Equal(13092, diagnostics.Port);
        Assert.Equal("diagdb", diagnostics.Database);
        Assert.False(diagnostics.Pooling);
        Assert.Null(diagnostics.Pool);
        Assert.Null(diagnostics.LastPlan);
        Assert.Null(diagnostics.LastSblr);
        Assert.NotNull(diagnostics.CircuitBreaker);
        Assert.False(diagnostics.CircuitBreaker.Enabled);
        Assert.Equal(CircuitBreakerState.Closed, diagnostics.CircuitBreaker.State);
        Assert.NotNull(diagnostics.Keepalive);
        Assert.True(diagnostics.Keepalive.Enabled);
        Assert.Equal(120000, diagnostics.Keepalive.IntervalMs);
        Assert.NotNull(diagnostics.Pipeline);
        Assert.True(diagnostics.Pipeline.Enabled);
        Assert.Equal(100, diagnostics.Pipeline.MaxInFlight);
        Assert.NotNull(diagnostics.LeakDetection);
        Assert.True(diagnostics.LeakDetection.Enabled);
        Assert.False(diagnostics.LeakDetection.ActiveCheckout);
    }

    [Fact]
    public void GetPoolDiagnostics_ReturnsMappedPoolCounters()
    {
        var dsn = BuildUniquePoolingDsn();
        var config = ScratchBirdConfig.FromConnectionString(dsn);
        _ = ProtocolClientPool.BorrowOrCreate(config, out var lease);
        lease.Dispose();

        var direct = ScratchBirdConnection.GetPoolDiagnostics(dsn);
        Assert.NotNull(direct);
        Assert.Equal(4, direct!.MaxSize);
        Assert.True(direct.BorrowAttempts >= 1);
        Assert.True(direct.Borrowed >= 1);
        Assert.True(direct.Rejected >= 1);

        using var connection = new ScratchBirdConnection(dsn);
        var viaConnection = connection.GetPoolDiagnostics();
        Assert.Equal(direct, viaConnection);
    }

    [Fact]
    public void GetDiagnostics_ExposesPlanAndSblrWithCopiedPayloads()
    {
        using var connection = new ScratchBirdConnection(
            "Host=diag.local;Port=13092;Database=diagdb;Username=app;Password=secret;Pooling=false");
        var protocolClient = new ProtocolClient();
        var planBytes = new byte[] { 1, 2, 3 };
        var sblrBytes = new byte[] { 4, 5, 6 };

        SetPrivateField(protocolClient, "_lastPlan", ((uint)1, (ulong)7, (ulong)11, (ulong)13, planBytes));
        SetPrivateField(protocolClient, "_lastSblr", ((ulong)17, (uint)2, sblrBytes));
        SetPrivateField(connection, "_client", protocolClient);
        SetPrivateField(connection, "_state", ConnectionState.Open);

        var first = connection.GetDiagnostics();
        Assert.NotNull(first.LastPlan);
        Assert.NotNull(first.LastSblr);
        Assert.Equal(new byte[] { 1, 2, 3 }, first.LastPlan!.Plan);
        Assert.Equal(new byte[] { 4, 5, 6 }, first.LastSblr!.Bytecode);

        first.LastPlan.Plan[0] = 99;
        first.LastSblr.Bytecode[0] = 99;

        var second = connection.GetDiagnostics();
        Assert.Equal(1, second.LastPlan!.Plan[0]);
        Assert.Equal(4, second.LastSblr!.Bytecode[0]);
    }

    private static string BuildUniquePoolingDsn()
    {
        var unique = Guid.NewGuid().ToString("N");
        return $"Host=127.0.0.1;Port=13092;Database=diag_pool_{unique};Username=diag_{unique};Password=secret;Pooling=true;MinPoolSize=0;MaxPoolSize=4;ConnectionLifetime=30";
    }

    private static void SetPrivateField(object target, string fieldName, object value)
    {
        var field = target.GetType().GetField(fieldName, BindingFlags.Instance | BindingFlags.NonPublic);
        Assert.NotNull(field);
        field!.SetValue(target, value);
    }
}
