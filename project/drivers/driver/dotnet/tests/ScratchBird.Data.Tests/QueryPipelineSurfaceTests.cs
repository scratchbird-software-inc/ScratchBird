// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System;
using System.Collections.Generic;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using ScratchBird.Data;
using Xunit;

namespace ScratchBird.Data.Tests;

public class QueryPipelineSurfaceTests
{
    [Fact]
    public async Task QueueAsync_AutoFlushThresholdProcessesRequests()
    {
        var executed = new List<string>();
        using var connection = new ScratchBirdConnection("Host=localhost;Port=3092;Database=main");
        await using var pipeline = new ScratchBirdQueryPipeline(
            connection,
            new ScratchBirdQueryPipelineConfig
            {
                MaxInFlight = 4,
                AutoFlush = true,
                AutoFlushThreshold = 2,
                FlushTimeoutMs = 60000
            },
            (sql, _, _, _, _) =>
            {
                lock (executed)
                {
                    executed.Add(sql);
                }

                return Task.FromResult(CreateResult(sql));
            });

        var first = pipeline.QueueAsync("SELECT 1");
        var second = pipeline.QueueAsync("SELECT 2");

        var results = await Task.WhenAll(first, second).WaitAsync(TimeSpan.FromSeconds(2));

        Assert.Equal(0, pipeline.PendingCount);
        Assert.Equal(0, pipeline.InFlightCount);
        Assert.Equal(2, executed.Count);
        Assert.Equal("SELECT 1", results[0][0].Command);
        Assert.Equal("SELECT 2", results[1][0].Command);
    }

    [Fact]
    public async Task QueueAsync_ManualFlushProcessesWhenAutoFlushDisabled()
    {
        using var connection = new ScratchBirdConnection("Host=localhost;Port=3092;Database=main");
        await using var pipeline = new ScratchBirdQueryPipeline(
            connection,
            new ScratchBirdQueryPipelineConfig
            {
                MaxInFlight = 4,
                AutoFlush = false,
                FlushTimeoutMs = 60000
            },
            (sql, _, _, _, _) => Task.FromResult(CreateResult(sql)));

        var pending = pipeline.QueueAsync("SELECT 1");
        await Task.Delay(100);
        Assert.False(pending.IsCompleted);

        pipeline.Flush();
        var result = await pending.WaitAsync(TimeSpan.FromSeconds(2));

        Assert.Single(result);
        Assert.Equal("SELECT 1", result[0].Command);
    }

    [Fact]
    public async Task QueueAsync_RejectsWhenAtCapacity()
    {
        using var connection = new ScratchBirdConnection("Host=localhost;Port=3092;Database=main");
        await using var pipeline = new ScratchBirdQueryPipeline(
            connection,
            new ScratchBirdQueryPipelineConfig
            {
                MaxInFlight = 1,
                AutoFlush = false,
                FlushTimeoutMs = 60000
            },
            async (sql, _, _, _, cancellationToken) =>
            {
                await Task.Delay(50, cancellationToken);
                return CreateResult(sql);
            });

        var first = pipeline.QueueAsync("SELECT 1");
        var second = pipeline.QueueAsync("SELECT 2");

        var ex = await Assert.ThrowsAsync<ScratchBirdLimitException>(async () => await second);
        Assert.Equal("54000", ex.SqlState);

        pipeline.Flush();
        await first.WaitAsync(TimeSpan.FromSeconds(2));
    }

    [Fact]
    public void CreateQueryPipeline_UsesConnectionPipelineDefaults()
    {
        using var connection = new ScratchBirdConnection(
            "Host=localhost;Port=3092;Database=main;pipeline_max_in_flight=3;" +
            "pipeline_auto_flush=0;pipeline_auto_flush_threshold=6;pipeline_flush_timeout_ms=42");
        using var pipeline = connection.CreateQueryPipeline();

        var config = GetPrivateField<ScratchBirdQueryPipelineConfig>(pipeline, "_config");
        Assert.Equal(3, config.MaxInFlight);
        Assert.False(config.AutoFlush);
        Assert.Equal(6, config.AutoFlushThreshold);
        Assert.Equal(42, config.FlushTimeoutMs);
    }

    [Fact]
    public async Task Batch_ExecuteAsync_QueuesAndReturnsOrderedResults()
    {
        var executed = new List<string>();
        using var connection = new ScratchBirdConnection("Host=localhost;Port=3092;Database=main");
        await using var pipeline = new ScratchBirdQueryPipeline(
            connection,
            new ScratchBirdQueryPipelineConfig
            {
                MaxInFlight = 4,
                AutoFlush = false,
                FlushTimeoutMs = 60000
            },
            (sql, _, _, _, _) =>
            {
                lock (executed)
                {
                    executed.Add(sql);
                }

                return Task.FromResult(CreateResult(sql));
            });

        var batch = pipeline.CreateBatch()
            .Add("SELECT 1")
            .Add("SELECT 2");

        var results = await batch.ExecuteAsync().WaitAsync(TimeSpan.FromSeconds(2));

        Assert.Equal(2, batch.Count);
        Assert.Equal(2, results.Count);
        Assert.Equal("SELECT 1", results[0][0].Command);
        Assert.Equal("SELECT 2", results[1][0].Command);
        Assert.Equal(new[] { "SELECT 1", "SELECT 2" }, executed);
    }

    [Fact]
    public async Task Batch_ExecuteAsync_ReturnsEmptyWhenNoItemsQueued()
    {
        using var connection = new ScratchBirdConnection("Host=localhost;Port=3092;Database=main");
        await using var pipeline = connection.CreateQueryPipeline();

        var results = await pipeline.CreateBatch().ExecuteAsync();
        Assert.Empty(results);
    }

    [Fact]
    public async Task Batch_ExecuteAsync_PropagatesCapacityErrors()
    {
        var executed = new List<string>();
        using var connection = new ScratchBirdConnection("Host=localhost;Port=3092;Database=main");
        await using var pipeline = new ScratchBirdQueryPipeline(
            connection,
            new ScratchBirdQueryPipelineConfig
            {
                MaxInFlight = 1,
                AutoFlush = false,
                FlushTimeoutMs = 60000
            },
            async (sql, _, _, _, cancellationToken) =>
            {
                lock (executed)
                {
                    executed.Add(sql);
                }
                await Task.Delay(50, cancellationToken);
                return CreateResult(sql);
            });

        var batch = pipeline.CreateBatch()
            .Add("SELECT 1")
            .Add("SELECT 2");

        var ex = await Assert.ThrowsAsync<ScratchBirdLimitException>(async () => await batch.ExecuteAsync());
        Assert.Equal("54000", ex.SqlState);
        Assert.Empty(executed);
        Assert.Equal(0, pipeline.PendingCount);
        Assert.Equal(0, pipeline.InFlightCount);
    }

    [Fact]
    public async Task ExecuteBatchAsync_UsesBatchItemConvenienceApi()
    {
        using var connection = new ScratchBirdConnection("Host=localhost;Port=3092;Database=main");
        await using var pipeline = new ScratchBirdQueryPipeline(
            connection,
            new ScratchBirdQueryPipelineConfig
            {
                MaxInFlight = 4,
                AutoFlush = false,
                FlushTimeoutMs = 60000
            },
            (sql, _, _, _, _) => Task.FromResult(CreateResult(sql)));

        var items = new[]
        {
            new ScratchBirdPipelineBatchItem("SELECT 1"),
            new ScratchBirdPipelineBatchItem("SELECT 2")
        };

        var results = await pipeline.ExecuteBatchAsync(items).WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal(2, results.Count);
        Assert.Equal("SELECT 1", results[0][0].Command);
        Assert.Equal("SELECT 2", results[1][0].Command);
    }

    [Fact]
    public async Task ConnectionExecutePipelineBatchAsync_ReturnsEmptyForEmptyBatch()
    {
        using var connection = new ScratchBirdConnection("Host=localhost;Port=3092;Database=main");
        var results = await connection.ExecutePipelineBatchAsync(Array.Empty<ScratchBirdPipelineBatchItem>());
        Assert.Empty(results);
    }

    private static IReadOnlyList<ResultSetSummary> CreateResult(string command)
    {
        return new[]
        {
            new ResultSetSummary(
                Array.Empty<object?[]>(),
                0,
                Array.Empty<FieldSummary>(),
                command,
                0)
        };
    }

    private static T GetPrivateField<T>(object target, string fieldName)
        where T : class
    {
        var field = target.GetType().GetField(fieldName, BindingFlags.Instance | BindingFlags.NonPublic);
        Assert.NotNull(field);
        var value = field!.GetValue(target);
        Assert.IsType<T>(value);
        return (T)value!;
    }
}
