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

public class TelemetrySurfaceTests
{
    [Fact]
    public void GetTelemetrySummary_WhenNoOperations_ReturnsEmptyTotals()
    {
        using var connection = new ScratchBirdConnection();

        var summary = connection.GetTelemetrySummary();

        Assert.Equal(0, summary.TotalInvocations);
        Assert.Equal(0, summary.TotalSuccesses);
        Assert.Equal(0, summary.TotalFailures);
        Assert.Empty(summary.Operations);
    }

    [Fact]
    public void RecordTelemetry_TracksOperationCounters()
    {
        using var connection = new ScratchBirdConnection();

        connection.RecordTelemetry("Command.ExecuteNonQuery", TimeSpan.FromMilliseconds(12), success: true);
        connection.RecordTelemetry("Command.ExecuteNonQuery", TimeSpan.FromMilliseconds(8), success: false);
        connection.RecordTelemetry("Connection.GetSchema", TimeSpan.FromMilliseconds(5), success: true);

        var summary = connection.GetTelemetrySummary();
        Assert.Equal(3, summary.TotalInvocations);
        Assert.Equal(2, summary.TotalSuccesses);
        Assert.Equal(1, summary.TotalFailures);

        var nonQuery = Assert.Single(summary.Operations, operation => operation.Operation == "Command.ExecuteNonQuery");
        Assert.Equal(2, nonQuery.Invocations);
        Assert.Equal(1, nonQuery.Successes);
        Assert.Equal(1, nonQuery.Failures);
        Assert.Equal(20, nonQuery.TotalDurationMs);
        Assert.Equal(12, nonQuery.MaxDurationMs);
        Assert.InRange(nonQuery.AverageDurationMs, 9.9d, 10.1d);
    }

    [Fact]
    public void CommandFailure_RecordsTelemetryOnConnection()
    {
        using var connection = new ScratchBirdConnection(
            "Host=localhost;Port=13092;Database=main;Username=sb_admin;Password=SbAdmin_Compat1!;Pooling=false");
        using var command = connection.CreateCommand();
        command.CommandText = "SELECT 1";

        Assert.Throws<InvalidOperationException>(() => command.ExecuteNonQuery());

        var summary = connection.GetTelemetrySummary();
        var operation = Assert.Single(summary.Operations, value => value.Operation == "Command.ExecuteNonQuery");
        Assert.Equal(1, operation.Invocations);
        Assert.Equal(0, operation.Successes);
        Assert.Equal(1, operation.Failures);
    }

    [Fact]
    public void ResetTelemetry_ClearsAllCounters()
    {
        using var connection = new ScratchBirdConnection();
        connection.ConnectionString = "Host=localhost;TelemetrySlowOperationThresholdMs=1";
        connection.RecordTelemetry("Connection.QueryMulti", TimeSpan.FromMilliseconds(10), success: true);
        Assert.NotEmpty(connection.GetTelemetrySummary().Operations);
        Assert.NotEmpty(connection.GetSlowOperations());

        connection.ResetTelemetry();
        var summary = connection.GetTelemetrySummary();

        Assert.Equal(0, summary.TotalInvocations);
        Assert.Empty(summary.Operations);
        Assert.Empty(connection.GetSlowOperations());
    }

    [Fact]
    public void SlowOperationsAndPrometheusExport_AreAvailable()
    {
        using var connection = new ScratchBirdConnection();
        connection.ConnectionString =
            "Host=localhost;TelemetrySlowOperationThresholdMs=5;TelemetrySlowOperationMaxEntries=2";

        connection.RecordTelemetry("Command.ExecuteNonQuery", TimeSpan.FromMilliseconds(3), success: true);
        connection.RecordTelemetry("Connection.Query", TimeSpan.FromMilliseconds(6), success: true);
        connection.RecordTelemetry("Connection.Query", TimeSpan.FromMilliseconds(9), success: false);
        connection.RecordTelemetry("Connection.Query", TimeSpan.FromMilliseconds(12), success: true);

        var slowOperations = connection.GetSlowOperations();
        Assert.Equal(2, slowOperations.Count);
        Assert.Equal(9, slowOperations[0].DurationMs);
        Assert.Equal(12, slowOperations[1].DurationMs);

        var metrics = connection.ExportTelemetryPrometheus();
        Assert.Contains("scratchbird_operations_total 4", metrics);
        Assert.Contains("scratchbird_operation_invocations_total{operation=\"Connection.Query\"} 3", metrics);
        Assert.Contains("scratchbird_slow_operations_queued 2", metrics);
    }

    [Fact]
    public void TelemetryTracingDisabled_SkipsCountersAndSlowOperationLog()
    {
        using var connection = new ScratchBirdConnection(
            "Host=localhost;TelemetryEnableTracing=false;TelemetrySlowOperationThresholdMs=1");

        connection.RecordTelemetry("Command.ExecuteScalar", TimeSpan.FromMilliseconds(15), success: true);

        var summary = connection.GetTelemetrySummary();
        Assert.Equal(0, summary.TotalInvocations);
        Assert.Empty(summary.Operations);
        Assert.Empty(connection.GetSlowOperations());
    }

    [Fact]
    public void SlowOperationStatements_AreSanitizedByDefault()
    {
        using var connection = new ScratchBirdConnection(
            "Host=localhost;TelemetrySlowOperationThresholdMs=1");

        connection.RecordTelemetry(
            "Command.ExecuteNonQuery",
            TimeSpan.FromMilliseconds(10),
            success: false,
            statement: "SELECT * FROM users WHERE token='secret-token' AND role='admin'");

        var slow = Assert.Single(connection.GetSlowOperations());
        Assert.Equal("SELECT * FROM users WHERE token='?' AND role='?'", slow.Statement);
    }

    [Fact]
    public void SlowOperationStatements_SanitizeCanBeDisabled()
    {
        using var connection = new ScratchBirdConnection(
            "Host=localhost;TelemetrySlowOperationThresholdMs=1;TelemetrySanitizeStatements=false");

        connection.RecordTelemetry(
            "Command.ExecuteScalar",
            TimeSpan.FromMilliseconds(10),
            success: true,
            statement: "SELECT * FROM users WHERE token='secret-token'");

        var slow = Assert.Single(connection.GetSlowOperations());
        Assert.Equal("SELECT * FROM users WHERE token='secret-token'", slow.Statement);
    }
}
