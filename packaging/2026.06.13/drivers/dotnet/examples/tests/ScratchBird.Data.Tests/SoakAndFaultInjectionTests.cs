// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System;
using System.Collections.Concurrent;
using System.Data;
using System.Linq;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using ScratchBird.Data;
using Xunit;

namespace ScratchBird.Data.Tests;

[Collection("ScratchBird Integration")]
public class SoakAndFaultInjectionTests
{
    [Fact]
    public async Task CancellationReleaseSoakHarness()
    {
        if (!IsEnabled("SCRATCHBIRD_DOTNET_SOAK_ENABLE"))
        {
            return;
        }

        var durationSeconds = GetIntEnv("SCRATCHBIRD_DOTNET_SOAK_SECONDS", defaultValue: 90, minValue: 5, maxValue: 86400);
        var minIterations = GetIntEnv("SCRATCHBIRD_DOTNET_SOAK_MIN_ITERATIONS", defaultValue: Math.Max(1, durationSeconds / 3), minValue: 1, maxValue: 1_000_000);
        var minVerifyReads = GetIntEnv("SCRATCHBIRD_DOTNET_SOAK_MIN_VERIFY_READS", defaultValue: 1, minValue: 0, maxValue: 1_000_000);
        var dsn = EnsureSocketTimeout(RequireDsn(), 30);
        var cancelSql = RequireCancelSql();
        var poolingDsn = AddPoolingFlags(dsn, maxPoolSize: 4, connectionLifetime: 30, minPoolSize: 0);
        var startedAt = DateTime.UtcNow;
        var until = startedAt.AddSeconds(durationSeconds);
        var initialStats = GetPoolStats(poolingDsn);
        var initialBorrowAttempts = initialStats?.BorrowAttempts ?? 0;
        var initialBorrowed = initialStats?.Borrowed ?? 0;
        var initialReturned = initialStats?.Returned ?? 0;

        var iterations = 0;
        var successfulVerifyReads = 0;
        var transientOrCancelled = 0;

        while (DateTime.UtcNow < until)
        {
            using var conn = new ScratchBirdConnection(poolingDsn);
            conn.Open();

            using (var cmd = conn.CreateCommand())
            {
                cmd.CommandText = cancelSql;
                cmd.CommandTimeout = 2;
                using var cts = new CancellationTokenSource(450);
                try
                {
                    await cmd.ExecuteNonQueryAsync(cts.Token);
                }
                catch (Exception ex) when (IsExpectedSoakException(ex))
                {
                    Interlocked.Increment(ref transientOrCancelled);
                }
            }

            using (var verify = conn.CreateCommand())
            {
                verify.CommandText = "SELECT 1";
                try
                {
                    var result = Convert.ToInt32(verify.ExecuteScalar());
                    if (result == 1)
                    {
                        Interlocked.Increment(ref successfulVerifyReads);
                    }
                }
                catch (ScratchBirdException ex) when (IsTransientIntegrationException(ex))
                {
                    Interlocked.Increment(ref transientOrCancelled);
                }
            }

            Interlocked.Increment(ref iterations);
            await Task.Yield();
        }

        var finalStats = WaitForIdlePool(poolingDsn, retries: 40, delayMs: 100);
        if (finalStats.HasValue)
        {
            Assert.Equal(0, finalStats.Value.ActiveCount);
            Assert.True(finalStats.Value.BorrowAttempts >= initialBorrowAttempts);
            Assert.True(finalStats.Value.Borrowed >= initialBorrowed);
            Assert.True(finalStats.Value.Returned >= initialReturned);
        }

        Assert.True(iterations >= minIterations, $"expected soak harness to execute at least {minIterations} iteration(s)");
        Assert.True(
            successfulVerifyReads >= minVerifyReads || transientOrCancelled > 0,
            $"expected at least {minVerifyReads} verification read(s) or one transient/cancel outcome");

        Console.WriteLine(
            $"DOTNET-101 soak summary: seconds={durationSeconds}, iterations={iterations}, verifyReads={successfulVerifyReads}, transientOrCancelled={transientOrCancelled}, minIterations={minIterations}, minVerifyReads={minVerifyReads}, activeCount={(finalStats?.ActiveCount ?? -1)}, borrowAttempts={(finalStats?.BorrowAttempts ?? -1)}, borrowed={(finalStats?.Borrowed ?? -1)}, returned={(finalStats?.Returned ?? -1)}, rejected={(finalStats?.Rejected ?? -1)}");
    }

    [Fact]
    public async Task FailoverSaturationRecoveryHarness()
    {
        if (!IsEnabled("SCRATCHBIRD_DOTNET_FAILOVER_SOAK_ENABLE"))
        {
            return;
        }

        var durationSeconds = GetIntEnv("SCRATCHBIRD_DOTNET_FAILOVER_SOAK_SECONDS", defaultValue: 60, minValue: 5, maxValue: 7200);
        var workerCount = GetIntEnv("SCRATCHBIRD_DOTNET_FAILOVER_WORKERS", defaultValue: 6, minValue: 2, maxValue: 32);
        var minSuccess = GetIntEnv("SCRATCHBIRD_DOTNET_FAILOVER_MIN_SUCCESS", defaultValue: Math.Max(1, workerCount), minValue: 1, maxValue: 1_000_000);

        var dsn = EnsureSocketTimeout(RequireDsn(), 20);
        var poolingDsn = AddPoolingFlags(dsn, maxPoolSize: 2, connectionLifetime: 45, minPoolSize: 0);

        var success = 0;
        var transient = 0;
        var hardFailures = new ConcurrentBag<Exception>();

        using var stop = new CancellationTokenSource(TimeSpan.FromSeconds(durationSeconds));
        var workerTasks = Enumerable.Range(0, workerCount).Select(worker => Task.Run(async () =>
        {
            var localIteration = 0;
            while (!stop.IsCancellationRequested)
            {
                try
                {
                    using var conn = new ScratchBirdConnection(poolingDsn);
                    conn.Open();

                    // Force stale handles periodically to validate reconnect and fallback behavior.
                    if (localIteration % 7 == 0)
                    {
                        var client = GetClient(conn);
                        client?.Close();
                    }

                    using var cmd = conn.CreateCommand();
                    cmd.CommandText = "SELECT 1";
                    cmd.CommandTimeout = 2;
                    var result = Convert.ToInt32(cmd.ExecuteScalar());
                    if (result == 1)
                    {
                        Interlocked.Increment(ref success);
                    }
                }
                catch (Exception ex) when (IsExpectedSoakException(ex))
                {
                    Interlocked.Increment(ref transient);
                }
                catch (Exception ex)
                {
                    hardFailures.Add(ex);
                }

                localIteration++;
                await Task.Delay(50);
            }
        }, stop.Token)).ToList();

        await Task.WhenAll(workerTasks);

        Assert.True(success >= minSuccess, $"expected at least {minSuccess} successful operation(s) in failover saturation harness");
        Assert.True(hardFailures.IsEmpty, string.Join(" | ", hardFailures.Select(ex => ex.GetType().Name + ": " + ex.Message)));

        var finalStats = WaitForIdlePool(poolingDsn, retries: 30, delayMs: 100);
        Assert.NotNull(finalStats);
        Assert.Equal(0, finalStats!.Value.ActiveCount);
        Assert.True(finalStats!.Value.BorrowAttempts > 0);

        Console.WriteLine(
            $"DOTNET-102 failover soak summary: seconds={durationSeconds}, workers={workerCount}, success={success}, transient={transient}, minSuccess={minSuccess}, activeCount={finalStats.Value.ActiveCount}, borrowAttempts={finalStats.Value.BorrowAttempts}, borrowed={finalStats.Value.Borrowed}, returned={finalStats.Value.Returned}, rejected={finalStats.Value.Rejected}");
    }

    [Fact]
    public async Task IsolationAndDeadlockFaultInjectionMatrixHarness()
    {
        if (!IsEnabled("SCRATCHBIRD_DOTNET_FAULT_MATRIX_ENABLE"))
        {
            return;
        }

        var rounds = GetIntEnv("SCRATCHBIRD_DOTNET_FAULT_MATRIX_ROUNDS", defaultValue: 4, minValue: 1, maxValue: 512);
        var dsn = EnsureSocketTimeout(RequireDsn(), 30);
        var table = $"dotnet_fault_matrix_{Guid.NewGuid():N}";

        using (var setup = new ScratchBirdConnection(dsn))
        {
            setup.Open();
            using var ddl = setup.CreateCommand();
            ddl.CommandTimeout = 15;
            ddl.CommandText = $"CREATE TABLE {table} (id INTEGER PRIMARY KEY, value INTEGER)";
            ddl.ExecuteNonQuery();
            ddl.CommandText = $"INSERT INTO {table} (id, value) VALUES (1, 10)";
            ddl.ExecuteNonQuery();
        }

        var matrixChecks = 0;
        var committedOutcomes = 0;
        var contendedOutcomes = 0;
        try
        {
            for (var round = 0; round < rounds; round++)
            {
                foreach (var isolation in new[] { IsolationLevel.ReadCommitted, IsolationLevel.Serializable })
                {
                    using var blockerConn = new ScratchBirdConnection(dsn);
                    blockerConn.Open();

                    using var blockerTx = blockerConn.BeginTransaction(isolation);
                    using (var blockerCmd = blockerConn.CreateCommand())
                    {
                        blockerCmd.Transaction = blockerTx;
                        blockerCmd.CommandTimeout = 2;
                        blockerCmd.CommandText = $"UPDATE {table} SET value = value + 1 WHERE id = 1";
                        blockerCmd.ExecuteNonQuery();
                    }

                    var contenderTask = Task.Run(async () =>
                    {
                        using var contenderConn = new ScratchBirdConnection(dsn);
                        contenderConn.Open();
                        using var contenderTx = contenderConn.BeginTransaction(isolation);
                        using var contenderCmd = contenderConn.CreateCommand();
                        contenderCmd.Transaction = contenderTx;
                        contenderCmd.CommandTimeout = 1;
                        contenderCmd.CommandText = $"UPDATE {table} SET value = value + 1 WHERE id = 1";
                        using var cts = new CancellationTokenSource(450);

                        try
                        {
                            await contenderCmd.ExecuteNonQueryAsync(cts.Token);
                            contenderTx.Commit();
                            return "committed";
                        }
                        catch (Exception ex) when (IsExpectedSoakException(ex))
                        {
                            SafeRollback(contenderTx);
                            return "contended";
                        }
                    });

                    await Task.Delay(300);
                    SafeRollback(blockerTx);

                    var outcome = await contenderTask;
                    Assert.True(outcome is "committed" or "contended", $"unexpected matrix outcome: {outcome}");
                    if (outcome == "committed")
                    {
                        committedOutcomes++;
                    }
                    else
                    {
                        contendedOutcomes++;
                    }

                    using var verifyConn = new ScratchBirdConnection(dsn);
                    verifyConn.Open();
                    using var verify = verifyConn.CreateCommand();
                    verify.CommandText = $"SELECT COUNT(*) FROM {table}";
                    Assert.True(Convert.ToInt32(verify.ExecuteScalar()) >= 1);

                    matrixChecks++;
                }
            }
        }
        finally
        {
            using var cleanup = new ScratchBirdConnection(dsn);
            cleanup.Open();
            using var drop = cleanup.CreateCommand();
            drop.CommandTimeout = 15;
            drop.CommandText = $"DROP TABLE IF EXISTS {table}";
            try
            {
                drop.ExecuteNonQuery();
            }
            catch (ScratchBirdException ex) when (IsTransientIntegrationException(ex))
            {
            }
        }

        Assert.True(matrixChecks >= rounds * 2, $"expected matrix coverage for configured isolation levels across {rounds} round(s)");
        Assert.True(committedOutcomes + contendedOutcomes == matrixChecks);
        Console.WriteLine(
            $"DOTNET-103 fault-matrix summary: rounds={rounds}, checks={matrixChecks}, committed={committedOutcomes}, contended={contendedOutcomes}");
    }

    private static bool IsEnabled(string environmentVariable)
    {
        var value = Environment.GetEnvironmentVariable(environmentVariable);
        if (string.IsNullOrWhiteSpace(value))
        {
            return false;
        }

        return value.Equals("1", StringComparison.OrdinalIgnoreCase)
            || value.Equals("true", StringComparison.OrdinalIgnoreCase)
            || value.Equals("yes", StringComparison.OrdinalIgnoreCase)
            || value.Equals("on", StringComparison.OrdinalIgnoreCase);
    }

    private static int GetIntEnv(string environmentVariable, int defaultValue, int minValue, int maxValue)
    {
        var raw = Environment.GetEnvironmentVariable(environmentVariable);
        if (!string.IsNullOrWhiteSpace(raw) && int.TryParse(raw, out var parsed))
        {
            return Math.Clamp(parsed, minValue, maxValue);
        }

        return Math.Clamp(defaultValue, minValue, maxValue);
    }

    private static ProtocolClientPool.PoolStats? WaitForIdlePool(string dsn, int retries, int delayMs)
    {
        ProtocolClientPool.PoolStats? stats = null;
        for (var attempt = 0; attempt < retries; attempt++)
        {
            stats = GetPoolStats(dsn);
            if (!stats.HasValue || stats.Value.ActiveCount == 0)
            {
                return stats;
            }

            Thread.Sleep(delayMs);
        }

        return stats;
    }

    private static bool IsExpectedSoakException(Exception exception)
    {
        if (exception is OperationCanceledException || exception is TimeoutException)
        {
            return true;
        }

        if (exception is ScratchBirdException sbException)
        {
            return IsTransientIntegrationException(sbException);
        }

        var message = exception.Message ?? string.Empty;
        return message.Contains("timed out", StringComparison.OrdinalIgnoreCase)
            || message.Contains("canceled", StringComparison.OrdinalIgnoreCase)
            || message.Contains("connection", StringComparison.OrdinalIgnoreCase)
            || message.Contains("transport", StringComparison.OrdinalIgnoreCase)
            || message.Contains("failed to send query", StringComparison.OrdinalIgnoreCase);
    }

    private static void SafeRollback(IDbTransaction transaction)
    {
        try
        {
            transaction.Rollback();
        }
        catch
        {
        }
    }

    private static ProtocolClient? GetClient(ScratchBirdConnection connection)
    {
        var field = typeof(ScratchBirdConnection).GetField("_client", BindingFlags.NonPublic | BindingFlags.Instance);
        return field?.GetValue(connection) as ProtocolClient;
    }

    private static ProtocolClientPool.PoolStats? GetPoolStats(string dsn)
    {
        var config = ScratchBirdConfig.FromConnectionString(dsn);
        return ProtocolClientPool.GetStats(config);
    }

    private static string AddPoolingFlags(string dsn, int maxPoolSize, int connectionLifetime, int minPoolSize)
    {
        if (dsn.Contains("://", StringComparison.OrdinalIgnoreCase))
        {
            return
                $"{dsn}{(dsn.Contains("?", StringComparison.OrdinalIgnoreCase) ? "&" : "?")}Pooling=true&MaxPoolSize={maxPoolSize}&ConnectionLifetime={connectionLifetime}&MinPoolSize={minPoolSize}";
        }

        if (dsn.EndsWith(';'))
        {
            return
                $"{dsn}Pooling=true;MaxPoolSize={maxPoolSize};ConnectionLifetime={connectionLifetime};MinPoolSize={minPoolSize}";
        }

        return $"{dsn};Pooling=true;MaxPoolSize={maxPoolSize};ConnectionLifetime={connectionLifetime};MinPoolSize={minPoolSize}";
    }

    private static string RequireDsn()
    {
        var configured = Environment.GetEnvironmentVariable("SCRATCHBIRD_DOTNET_URL");
        var dsn = string.IsNullOrWhiteSpace(configured)
            ? "scratchbird://sb_admin:SbAdmin_Compat1!@127.0.0.1:13092/main?sslmode=disable&allow_insecure=true"
            : configured;
        return EnsureSocketTimeout(EnsurePoolingDisabled(dsn), 60);
    }

    private static string EnsurePoolingDisabled(string dsn)
    {
        if (dsn.Contains("://", StringComparison.OrdinalIgnoreCase))
        {
            return $"{dsn}{(dsn.Contains("?", StringComparison.OrdinalIgnoreCase) ? "&" : "?")}Pooling=false";
        }

        if (dsn.EndsWith(';'))
        {
            return $"{dsn}Pooling=false";
        }

        return $"{dsn};Pooling=false";
    }

    private static string EnsureSocketTimeout(string dsn, int seconds)
    {
        if (seconds <= 0)
        {
            return dsn;
        }

        if (dsn.IndexOf("socket_timeout", StringComparison.OrdinalIgnoreCase) >= 0
            || dsn.IndexOf("sockettimeout", StringComparison.OrdinalIgnoreCase) >= 0)
        {
            return dsn;
        }

        if (dsn.Contains("://", StringComparison.OrdinalIgnoreCase))
        {
            return $"{dsn}{(dsn.Contains("?", StringComparison.OrdinalIgnoreCase) ? "&" : "?")}socket_timeout={seconds}";
        }

        if (dsn.EndsWith(';'))
        {
            return $"{dsn}socket_timeout={seconds}";
        }

        return $"{dsn};socket_timeout={seconds}";
    }

    private static bool IsTransientIntegrationException(ScratchBirdException ex)
    {
        var message = ex.Message ?? string.Empty;
        return message.Contains("Connection lost", StringComparison.OrdinalIgnoreCase)
            || message.Contains("Failed to send query", StringComparison.OrdinalIgnoreCase)
            || message.Contains("query canceled", StringComparison.OrdinalIgnoreCase)
            || message.Contains("timeout", StringComparison.OrdinalIgnoreCase);
    }

    private static string RequireCancelSql()
    {
        var configured = Environment.GetEnvironmentVariable("SCRATCHBIRD_DOTNET_CANCEL_SQL");
        if (!string.IsNullOrWhiteSpace(configured))
        {
            return configured;
        }

        return "SELECT pg_sleep(5)";
    }
}
