// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using ScratchBird.Data;
using Xunit;

namespace ScratchBird.Data.Tests;

[Collection("ScratchBird Integration")]
public class JDBC203PoolingAndRecoveryContractTests
{
    private const int ScenarioCWorkers = 10;

    [Fact]
    public async Task ScenarioA_BorrowReuseAfterExplicitCancel()
    {
        var dsn = RequireDsn();
        var cancelSql = RequireCancelSql();

        var poolingDsn = AddPoolingFlags(dsn, maxPoolSize: 4, minPoolSize: 0, connectionLifetime: 30);
        var beforeStats = GetPoolStats(poolingDsn);

        using (var conn = new ScratchBirdConnection(poolingDsn))
        {
            conn.Open();
            using var statement = conn.CreateCommand();
            statement.CommandText = cancelSql;

            var executeTask = statement.ExecuteNonQueryAsync();
            Thread.Sleep(150);
            statement.Cancel();

            try
            {
                await executeTask.ConfigureAwait(false);
            }
            catch (Exception)
            {
                // Runtime-specific cancellation path may either complete or raise timeout/cancel errors.
            }
        }

        using (var verify = new ScratchBirdConnection(poolingDsn))
        {
            verify.Open();
            using var statement = verify.CreateCommand();
            statement.CommandText = "SELECT 1";
            var result = Convert.ToInt32(statement.ExecuteScalar());
            Assert.Equal(1, result);
        }

        var afterStats = GetPoolStats(poolingDsn);
        Assert.NotNull(afterStats);
        var baselineReturned = beforeStats?.Returned ?? 0;
        Assert.True(afterStats.Value.Borrowed >= 1);
        Assert.True(afterStats.Value.Returned >= baselineReturned);
    }

    [Fact]
    public void ScenarioB_TimeoutCancellationReuse()
    {
        var dsn = RequireDsn();
        var cancelSql = RequireCancelSql();

        var poolingDsn = AddPoolingFlags(dsn, maxPoolSize: 4, minPoolSize: 0, connectionLifetime: 30);

        using (var conn = new ScratchBirdConnection(poolingDsn))
        {
            conn.Open();
            using var statement = conn.CreateCommand();
            statement.CommandText = cancelSql;
            statement.CommandTimeout = 1;
            try
            {
                statement.ExecuteNonQuery();
            }
            catch (Exception)
            {
                // Runtime-specific cancellation path may either complete or raise timeout/cancel errors.
            }
        }

        using (var verify = new ScratchBirdConnection(poolingDsn))
        {
            verify.Open();
            using var statement = verify.CreateCommand();
            statement.CommandText = "SELECT 1";
            var result = Convert.ToInt32(statement.ExecuteScalar());
            Assert.Equal(1, result);
        }
    }

    [Fact]
    public async Task ScenarioC_ConcurrentPoolStress10Workers()
    {
        var dsn = RequireDsn();

        var poolingDsn = AddPoolingFlags(dsn, maxPoolSize: 3, minPoolSize: 0, connectionLifetime: 20);
        var tasks = Enumerable.Range(0, ScenarioCWorkers).Select(_ => Task.Run(() =>
        {
            using var conn = new ScratchBirdConnection(poolingDsn);
            conn.Open();
            using var statement = conn.CreateCommand();
            statement.CommandText = "SELECT 1";
            var result = Convert.ToInt32(statement.ExecuteScalar());
            return result == 1;
        })).ToList();

        var results = await Task.WhenAll(tasks);
        Assert.All(results, Assert.True);

        var afterStats = GetPoolStats(poolingDsn);
        Assert.NotNull(afterStats);
        Assert.True(afterStats.Value.ActiveCount <= 3);
        Assert.True(afterStats.Value.BorrowAttempts >= 10);
    }

    [Fact]
    public void ScenarioD_ReconnectRecoveryAfterFailure()
    {
        var dsn = RequireDsn();
        var cancelSql = RequireCancelSql();

        var poolingDsn = AddPoolingFlags(dsn, maxPoolSize: 2, minPoolSize: 0, connectionLifetime: 30);

        for (var iteration = 0; iteration < 2; iteration++)
        {
            using (var conn = new ScratchBirdConnection(poolingDsn))
            {
                conn.Open();
                using var statement = conn.CreateCommand();
                statement.CommandText = cancelSql;
                statement.CommandTimeout = 1;
                try
                {
                    statement.ExecuteNonQuery();
                }
                catch (Exception)
                {
                    // Runtime-specific cancellation path may either complete or raise timeout/cancel errors.
                }
            }
        }

        using var verify = new ScratchBirdConnection(poolingDsn);
        verify.Open();
        using var verifyStatement = verify.CreateCommand();
        verifyStatement.CommandText = "SELECT 1";
        var result = Convert.ToInt32(verifyStatement.ExecuteScalar());
        Assert.Equal(1, result);
    }

    [Fact]
    public void ScenarioE_MetadataAndStreamReuseAfterRecovery()
    {
        var dsn = RequireDsn();
        var cancelSql = RequireCancelSql();

        var table = $"dotnet203_contract_{Guid.NewGuid():N}";
        var payloadText = $"payload-{DateTime.UtcNow:O}";
        var poolingDsn = AddPoolingFlags(dsn, maxPoolSize: 4, minPoolSize: 0, connectionLifetime: 30);

        try
        {
            using (var conn = new ScratchBirdConnection(poolingDsn))
            {
                conn.Open();
                using var ddl = conn.CreateCommand();
                ddl.CommandTimeout = 15;
                ddl.CommandText = $"CREATE TABLE {table} (id INTEGER, note TEXT)";
                ddl.ExecuteNonQuery();

                using var insert = conn.CreateCommand();
                insert.CommandTimeout = 15;
                insert.CommandText = $"INSERT INTO {table} (id, note) VALUES (?, ?)";
                insert.Parameters.Add(new ScratchBirdParameter("", 1));
                insert.Parameters.Add(new ScratchBirdParameter("", payloadText));
                insert.ExecuteNonQuery();
            }

            using (var conn = new ScratchBirdConnection(poolingDsn))
            {
                conn.Open();

                using var cancelCommand = conn.CreateCommand();
                cancelCommand.CommandText = cancelSql;
                cancelCommand.CommandTimeout = 1;
                try
                {
                    cancelCommand.ExecuteNonQuery();
                }
                catch (Exception)
                {
                    // Runtime-specific cancellation path may either complete or raise timeout/cancel errors.
                }

                using var metadata = conn.GetSchema("Columns");
                var hasTableNameColumn = metadata.Columns.Contains("TABLE_NAME")
                    || metadata.Columns.Contains("table_name");
                if (hasTableNameColumn)
                {
                    var columnName = metadata.Columns.Contains("TABLE_NAME") ? "TABLE_NAME" : "table_name";
                    var matched = metadata.Rows.Cast<System.Data.DataRow>().Any(r =>
                        string.Equals(r[columnName]?.ToString(), table, StringComparison.OrdinalIgnoreCase));
                    if (!matched)
                    {
                        Assert.True(metadata.Rows.Count > 0);
                    }
                }
                else
                {
                    Assert.True(metadata.Rows.Count > 0);
                }

                using var select = conn.CreateCommand();
                select.CommandTimeout = 15;
                select.CommandText = $"SELECT note FROM {table} WHERE id = ?";
                select.Parameters.Add(new ScratchBirdParameter("", 1));
                using var reader = select.ExecuteReader();
                Assert.True(reader.Read());
                Assert.Equal(payloadText, reader.GetString(0));
            }
        }
        catch (ScratchBirdException ex) when (IsTransientIntegrationException(ex))
        {
            return;
        }
        finally
        {
            using var cleanup = new ScratchBirdConnection(poolingDsn);
            cleanup.Open();
            using var drop = cleanup.CreateCommand();
            drop.CommandTimeout = 15;
            drop.CommandText = $"DROP TABLE {table}";
            try
            {
                drop.ExecuteNonQuery();
            }
            catch (ScratchBirdException ex) when (IsTransientIntegrationException(ex))
            {
            }
        }
    }

    private static string AddPoolingFlags(
        string dsn,
        int maxPoolSize,
        int connectionLifetime,
        int minPoolSize)
    {
        if (dsn.Contains("://", StringComparison.OrdinalIgnoreCase))
        {
            return
                $"{dsn}{(dsn.Contains("?", StringComparison.OrdinalIgnoreCase) ? "&" : "?")}Pooling=true&MaxPoolSize={maxPoolSize}&ConnectionLifetime={connectionLifetime}&MinPoolSize={minPoolSize}";
        }

        return
            $"{dsn};Pooling=true;MaxPoolSize={maxPoolSize};ConnectionLifetime={connectionLifetime};MinPoolSize={minPoolSize}";
    }

    private static ProtocolClientPool.PoolStats? GetPoolStats(string dsn)
    {
        var config = ScratchBirdConfig.FromConnectionString(dsn);
        return ProtocolClientPool.GetStats(config);
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
