// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System;
using ScratchBird.Data;
using Xunit;

namespace ScratchBird.Data.Tests;

public class SubscriptionSurfaceTests
{
    [Fact]
    public void NormalizeNotificationChannel_TrimmedValueIsReturned()
    {
        var normalized = ScratchBirdConnection.NormalizeNotificationChannel("  channel.events  ");
        Assert.Equal("channel.events", normalized);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("   ")]
    [InlineData("chan\0nel")]
    public void NormalizeNotificationChannel_InvalidInputThrows(string? channel)
    {
        Assert.Throws<ArgumentException>(() => ScratchBirdConnection.NormalizeNotificationChannel(channel));
    }

    [Fact]
    public void Listen_WhenClosedRecordsFailureTelemetry()
    {
        using var connection = new ScratchBirdConnection();

        Assert.Throws<InvalidOperationException>(() => connection.Listen("alerts"));

        var summary = connection.GetTelemetrySummary();
        var operation = Assert.Single(summary.Operations, value => value.Operation == "Connection.Subscribe");
        Assert.Equal(1, operation.Invocations);
        Assert.Equal(0, operation.Successes);
        Assert.Equal(1, operation.Failures);
    }

    [Fact]
    public void Unlisten_WhenClosedRecordsFailureTelemetry()
    {
        using var connection = new ScratchBirdConnection();

        Assert.Throws<InvalidOperationException>(() => connection.Unlisten("alerts"));

        var summary = connection.GetTelemetrySummary();
        var operation = Assert.Single(summary.Operations, value => value.Operation == "Connection.Unsubscribe");
        Assert.Equal(1, operation.Invocations);
        Assert.Equal(0, operation.Successes);
        Assert.Equal(1, operation.Failures);
    }

    [Fact]
    public void BuildNotifyCommand_QuotesChannelAndPayload()
    {
        var sql = ScratchBirdConnection.BuildNotifyCommand(" chan\"nel ", "O'Reilly");
        Assert.Equal("NOTIFY \"chan\"\"nel\", 'O''Reilly'", sql);
    }

    [Fact]
    public void BuildNotifyCommand_RejectsPayloadWithNul()
    {
        Assert.Throws<ArgumentException>(() => ScratchBirdConnection.BuildNotifyCommand("alerts", "bad\0payload"));
    }

    [Fact]
    public void NotifyChannel_WhenClosedRecordsFailureTelemetry()
    {
        using var connection = new ScratchBirdConnection();

        Assert.Throws<InvalidOperationException>(() => connection.NotifyChannel("alerts"));

        var summary = connection.GetTelemetrySummary();
        var operation = Assert.Single(summary.Operations, value => value.Operation == "Connection.NotifyChannel");
        Assert.Equal(1, operation.Invocations);
        Assert.Equal(0, operation.Successes);
        Assert.Equal(1, operation.Failures);
    }

    [Fact]
    public void UnlistenAll_WhenClosedRecordsFailureTelemetry()
    {
        using var connection = new ScratchBirdConnection();

        Assert.Throws<InvalidOperationException>(() => connection.UnlistenAll());

        var summary = connection.GetTelemetrySummary();
        var operation = Assert.Single(summary.Operations, value => value.Operation == "Connection.UnlistenAll");
        Assert.Equal(1, operation.Invocations);
        Assert.Equal(0, operation.Successes);
        Assert.Equal(1, operation.Failures);
    }
}
