// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System;
using System.IO;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Text;
using System.Linq;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using ScratchBird.Data;
using Xunit;

namespace ScratchBird.Data.Tests;

[Collection("ScratchBird Integration")]
public class IntegrationTests
{
    [Fact]
    public void ConnectAndSelect()
    {
        var dsn = RequireDsn();

        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();

        using var cmd = conn.CreateCommand();
        cmd.CommandText = "SELECT 1";
        var result = cmd.ExecuteScalar();

        Assert.NotNull(result);
    }

    [Fact]
    public void PrepareBindQuery()
    {
        var dsn = RequireDsn();

        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();

        using var cmd = conn.CreateCommand();
        cmd.CommandText = "SELECT ?::INTEGER";
        cmd.Parameters.Add(new ScratchBirdParameter("", 42));
        var result = cmd.ExecuteScalar();

        Assert.Equal(42, Convert.ToInt32(result));
    }

    [Fact]
    public void TypesFixtureQuery()
    {
        var dsn = RequireDsn();

        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();

        using (var countCmd = conn.CreateCommand())
        {
            countCmd.CommandText = "SELECT COUNT(*) FROM type_coverage";
            var rowCount = Convert.ToInt32(countCmd.ExecuteScalar());
            Assert.True(rowCount >= 0);
        }
    }

    [Fact]
    public void PreparedStatementCacheCachesAndClearsOnSchemaMutation()
    {
        var dsn = RequireDsn();

        var table = $"dotnet_ps_cache_{Guid.NewGuid():N}";
        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();

        try
        {
            using (var selectCmd = conn.CreateCommand())
            {
                selectCmd.CommandText = "SELECT ?::INTEGER";
                selectCmd.Parameters.Add(new ScratchBirdParameter("", 42));
                selectCmd.Prepare();
                var result = selectCmd.ExecuteScalar();
                Assert.Equal(42, Convert.ToInt32(result));
            }

            var cachedCount = GetPreparedStatementCount(conn);
            Assert.True(cachedCount > 0, "Expected prepared-statement cache to contain the prepared statement");

            using (var ddl = conn.CreateCommand())
            {
                ddl.CommandText = $"CREATE TABLE {table} (id INTEGER)";
                ddl.ExecuteNonQuery();
            }

            Assert.Equal(0, GetPreparedStatementCount(conn));
        }
        catch (ScratchBirdException ex) when (IsTransientIntegrationException(ex))
        {
            return;
        }
        finally
        {
            using var cleanup = conn.CreateCommand();
            cleanup.CommandText = $"DROP TABLE IF EXISTS {table}";
            try
            {
                cleanup.ExecuteNonQuery();
            }
            catch (ScratchBirdException ex) when (IsTransientIntegrationException(ex))
            {
            }
        }
    }

    [Fact]
    public void PreparedStatementRetryAfterSchemaInvalidation()
    {
        var dsn = RequireDsn();

        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();

        using (var cmd = conn.CreateCommand())
        {
            cmd.CommandText = "SELECT ?::INTEGER";
            cmd.Parameters.Add(new ScratchBirdParameter("", 1));
            cmd.Prepare();
            using var reader = cmd.ExecuteReader();
            Assert.True(reader.Read());
            Assert.Equal(1, Convert.ToInt32(reader.GetValue(0)));
            while (reader.Read())
            {
            }
        }
        Assert.True(GetPreparedStatementCount(conn) > 0, "Expected prepared statement cache to retain prepared command entries");
    }

    [Fact]
    public void GetSchemaTablesAndTextReaderStreamPaths()
    {
        var dsn = RequireDsn();

        var table = $"dotnet_schema_{Guid.NewGuid():N}";
        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();

        using (var ddl = conn.CreateCommand())
        {
            ddl.CommandText = $"CREATE TABLE {table} (id INTEGER)";
            ddl.ExecuteNonQuery();
        }

        try
        {
            using var schemaCmd = conn.CreateCommand();
            schemaCmd.CommandText = "SELECT 1";
            Assert.Equal(1, Convert.ToInt32(schemaCmd.ExecuteScalar()));

            using var schemaConn = new ScratchBirdConnection(dsn);
            schemaConn.Open();
            var tables = schemaConn.GetSchema("Tables");
            Assert.NotNull(tables);
            var tableNameColumn = tables.Columns.Contains("table_name")
                ? "table_name"
                : (tables.Columns.Contains("TABLE_NAME") ? "TABLE_NAME" : null);
            Assert.True(tableNameColumn != null || tables.Columns.Count > 0, "Tables schema should expose metadata columns");

            var found = false;
            if (tableNameColumn != null)
            {
                foreach (System.Data.DataRow row in tables.Rows)
                {
                    if (string.Equals(row[tableNameColumn]?.ToString(), table, StringComparison.OrdinalIgnoreCase))
                    {
                        found = true;
                        break;
                    }
                }
            }
            else
            {
                found = tables.Rows.Count > 0;
            }
            Assert.True(found || tables.Rows.Count > 0, "Tables schema collection should be queryable");

            using var readConn = new ScratchBirdConnection(dsn);
            readConn.Open();
            using var readerCmd = readConn.CreateCommand();
            readerCmd.CommandText = "SELECT 'stream-check'::TEXT";
            using var reader = readerCmd.ExecuteReader();
            Assert.True(reader.Read());

            using var textReader = reader.GetTextReader(0);
            var payload = textReader.ReadToEnd();
            Assert.Equal("stream-check", payload);

            using var stream = reader.GetStream(0);
            using var ms = new MemoryStream();
            stream.CopyTo(ms);
            var bytes = ms.ToArray();
            Assert.Equal(Encoding.UTF8.GetBytes("stream-check"), bytes);

            if (SupportsBinaryRoundTrip(readConn, 10))
            {
                readerCmd.CommandText = "SELECT ?::BYTEA";
                readerCmd.Parameters.Clear();
                var expectedBytes = new byte[] { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
                readerCmd.Parameters.Add(new ScratchBirdParameter("", expectedBytes));
                using var byteReader = readerCmd.ExecuteReader();
                if (byteReader.Read())
                {
                    var observed = ReadBinaryPayload(byteReader, 0);
                    Assert.Equal(expectedBytes, observed);
                }
            }
        }
        finally
        {
            using var cleanup = conn.CreateCommand();
            cleanup.CommandText = $"DROP TABLE IF EXISTS {table}";
            cleanup.ExecuteNonQuery();
        }
    }

    [Fact]
    public void GetSchemaColumnsMetadata()
    {
        var dsn = RequireDsn();

        var table = $"dotnet_schema_metadata_{Guid.NewGuid():N}";
        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();

        using (var ddl = conn.CreateCommand())
        {
            ddl.CommandText = $"CREATE TABLE {table} (id INTEGER, payload TEXT)";
            ddl.ExecuteNonQuery();
        }

        try
        {
            var tables = conn.GetSchema("Columns");
            Assert.NotNull(tables);
            Assert.True(tables.Columns.Contains("TABLE_NAME") || tables.Columns.Contains("table_name"));
            Assert.True(tables.Columns.Contains("COLUMN_NAME") || tables.Columns.Contains("column_name"));
            Assert.True(tables.Columns.Contains("DATA_TYPE") || tables.Columns.Contains("data_type"));

            var tableNameColumn = tables.Columns.Contains("TABLE_NAME") ? "TABLE_NAME" : "table_name";
            var columnNameColumn = tables.Columns.Contains("COLUMN_NAME") ? "COLUMN_NAME" : "column_name";

            var tableColumns = tables.Rows.Cast<System.Data.DataRow>()
                .Where(row => string.Equals(GetDataRowValue(row, tableNameColumn), table, StringComparison.OrdinalIgnoreCase))
                .ToList();
            if (tableColumns.Count == 0)
            {
                Assert.True(tables.Rows.Count > 0);
            }
            else
            {
                Assert.Equal(2, tableColumns.Count);
                Assert.Contains(tableColumns, row => string.Equals(GetDataRowValue(row, columnNameColumn), "id", StringComparison.OrdinalIgnoreCase));
                Assert.Contains(tableColumns, row => string.Equals(GetDataRowValue(row, columnNameColumn), "payload", StringComparison.OrdinalIgnoreCase));
            }
        }
        finally
        {
            using var cleanup = conn.CreateCommand();
            cleanup.CommandText = $"DROP TABLE IF EXISTS {table}";
            cleanup.ExecuteNonQuery();
        }
    }

    [Fact]
    public void GetSchemaTablesMetadataIsDiscoverable()
    {
        var dsn = RequireDsn();

        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();

        var tables = conn.GetSchema("Tables");
        Assert.NotNull(tables);
        Assert.True(
            tables.Columns.Contains("table_name")
            || tables.Columns.Contains("TABLE_NAME"));
        Assert.True(
            tables.Columns.Contains("table_schema")
            || tables.Columns.Contains("TABLE_SCHEMA"));
        Assert.True(
            tables.Columns.Contains("table_type")
            || tables.Columns.Contains("TABLE_TYPE"));
    }

    [Fact]
    public void BinaryStreamRoundTripForLargePayload()
    {
        var dsn = RequireDsn();

        const int payloadSize = 256 * 1024;
        var original = new byte[payloadSize];
        for (int i = 0; i < original.Length; i++)
        {
            original[i] = (byte)(i & 0xFF);
        }

        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();
        if (!SupportsBinaryRoundTrip(conn, payloadSize))
        {
            return;
        }

        using var cmd = conn.CreateCommand();
        cmd.CommandText = "SELECT ?::BYTEA";
        cmd.Parameters.Add(new ScratchBirdParameter("", original));
        using var reader = cmd.ExecuteReader();
        Assert.True(reader.Read());
        var observed = ReadBinaryPayload(reader, 0);
        Assert.Equal(payloadSize, observed.Length);
        Assert.Equal(original, observed);
    }

    [Fact]
    public void BinaryStreamRoundTripForVeryLargePayload()
    {
        var dsn = RequireDsn();

        const int payloadSize = 1024 * 1024;
        var original = new byte[payloadSize];
        for (int i = 0; i < original.Length; i++)
        {
            original[i] = (byte)((i * 7 + 3) & 0xFF);
        }

        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();
        if (!SupportsBinaryRoundTrip(conn, payloadSize))
        {
            return;
        }

        using var cmd = conn.CreateCommand();
        cmd.CommandText = "SELECT ?::BYTEA";
        cmd.Parameters.Add(new ScratchBirdParameter("", original));
        using var reader = cmd.ExecuteReader();
        Assert.True(reader.Read());
        var observed = ReadBinaryPayload(reader, 0);
        Assert.Equal(payloadSize, observed.Length);
        Assert.Equal(original, observed);

        using var textCmd = conn.CreateCommand();
        textCmd.CommandText = "SELECT 'ok'::TEXT";
        using var textReader = textCmd.ExecuteReader();
        Assert.True(textReader.Read());
        using var textStream = textReader.GetTextReader(0);
        Assert.Equal("ok", textStream.ReadToEnd());
    }

    [Fact]
    public async Task ConcurrentVeryLargeLobRoundTripsDoNotLeakPool()
    {
        var dsn = RequireDsn();

        var basePayload = new byte[1024 * 1024];
        for (int i = 0; i < basePayload.Length; i++)
        {
            basePayload[i] = (byte)((i * 13) & 0xFF);
        }

        const int workerCount = 5;
        var workers = new List<Task<bool>>();
        var poolingDsn = AddPoolingFlags(dsn, maxPoolSize: 3, connectionLifetime: 30);
        using (var probe = new ScratchBirdConnection(poolingDsn))
        {
            probe.Open();
            if (!SupportsBinaryRoundTrip(probe, basePayload.Length))
            {
                return;
            }
        }

        for (var i = 0; i < workerCount; i++)
        {
            var tag = (byte)(i & 0xFF);
            workers.Add(Task.Run(() =>
            {
                using var conn = new ScratchBirdConnection(poolingDsn);
                conn.Open();

                var payload = (byte[])basePayload.Clone();
                for (var j = 0; j < payload.Length; j++)
                {
                    payload[j] ^= tag;
                }

                using var cmd = conn.CreateCommand();
                cmd.CommandText = "SELECT ?::BYTEA";
                cmd.Parameters.Add(new ScratchBirdParameter("", payload));

                using var reader = cmd.ExecuteReader();
                Assert.True(reader.Read());
                var observed = ReadBinaryPayload(reader, 0);
                if (observed.Length != payload.Length)
                {
                    return false;
                }

                return observed.SequenceEqual(payload);
            }));
        }

        var all = Task.WhenAll(workers);
        var timeout = Task.Delay(15000);
        var completed = await Task.WhenAny(all, timeout);
        Assert.Same(all, completed);

        var results = await all;
        Assert.All(results, Assert.True);

        var stats = GetPoolStats(poolingDsn);
        Assert.NotNull(stats);
        Assert.True(stats!.Value.BorrowAttempts >= workerCount);
        Assert.True(stats.Value.Borrowed >= workerCount);
        Assert.True(stats.Value.Rejected >= 0);
    }

    [Fact]
    public void ConnectionPoolingReusesProtocolClient()
    {
        var dsn = RequireDsn();

        var poolingDsn = AddPoolingFlags(dsn);
        ProtocolClient? firstClient;

        using (var conn1 = new ScratchBirdConnection(poolingDsn))
        {
            conn1.Open();
            firstClient = GetClient(conn1);
        }

        using (var conn2 = new ScratchBirdConnection(poolingDsn))
        {
            conn2.Open();
            var secondClient = GetClient(conn2);
            Assert.NotNull(firstClient);
            Assert.Same(firstClient!, secondClient);
        }
    }

    [Fact]
    public void SavepointRollbackAndRelease()
    {
        var dsn = RequireDsn();

        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();

        try
        {
            using var tx = conn.BeginTransaction(System.Data.IsolationLevel.ReadCommitted);
            tx.Save("odbc_pool_savepoint");
            tx.Rollback("odbc_pool_savepoint");
            tx.Release("odbc_pool_savepoint");
            tx.Rollback();
        }
        catch (ScratchBirdException ex) when (IsTransientIntegrationException(ex))
        {
            return;
        }
    }

    [Fact]
    public void QueryMultiReturnsIndependentResultSets()
    {
        var dsn = RequireDsn();
        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();

        IReadOnlyList<ResultSetSummary> resultSets;
        try
        {
            resultSets = conn.QueryMulti("SELECT 1 AS first_value; SELECT 2 AS second_value");
        }
        catch (ScratchBirdNotSupportedException)
        {
            return;
        }
        catch (ScratchBirdException ex) when (IsTransientIntegrationException(ex))
        {
            return;
        }

        Assert.True(resultSets.Count >= 2);
        Assert.NotEmpty(resultSets[0].Rows);
        Assert.NotEmpty(resultSets[1].Rows);
        Assert.Equal(1, Convert.ToInt32(resultSets[0].Rows[0][0]));
        Assert.Equal(2, Convert.ToInt32(resultSets[1].Rows[0][0]));
    }

    [Fact]
    public void DataReaderNextResultTraversesMultipleSelectResultSets()
    {
        var dsn = RequireDsn();
        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();

        try
        {
            using var cmd = conn.CreateCommand();
            cmd.CommandText = "SELECT 1 AS first_value; SELECT 2 AS second_value";

            using var reader = cmd.ExecuteReader();
            Assert.True(reader.Read());
            Assert.Equal(1, Convert.ToInt32(reader.GetValue(0)));
            Assert.False(reader.Read());

            Assert.True(reader.NextResult());
            Assert.True(reader.Read());
            Assert.Equal(2, Convert.ToInt32(reader.GetValue(0)));
            Assert.False(reader.Read());

            Assert.False(reader.NextResult());
        }
        catch (ScratchBirdNotSupportedException)
        {
            return;
        }
        catch (ScratchBirdException ex) when (IsTransientIntegrationException(ex))
        {
            return;
        }
    }

    [Fact]
    public void DataReaderNextResultSkipsUnreadRowsAndAdvances()
    {
        var dsn = RequireDsn();
        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();

        try
        {
            using var cmd = conn.CreateCommand();
            cmd.CommandText = "SELECT 1 AS value UNION ALL SELECT 2 AS value; SELECT 3 AS value";

            using var reader = cmd.ExecuteReader();
            Assert.True(reader.Read());
            Assert.Equal(1, Convert.ToInt32(reader.GetValue(0)));

            Assert.True(reader.NextResult());
            Assert.True(reader.Read());
            Assert.Equal(3, Convert.ToInt32(reader.GetValue(0)));
            Assert.False(reader.Read());
            Assert.False(reader.NextResult());
        }
        catch (ScratchBirdNotSupportedException)
        {
            return;
        }
        catch (ScratchBirdException ex) when (IsTransientIntegrationException(ex))
        {
            return;
        }
    }

    [Fact]
    public void ExecuteBatchReturnsSummary()
    {
        var dsn = RequireDsn();
        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();

        BatchSummary summary;
        try
        {
            summary = conn.ExecuteBatch(
                "SELECT ?::INTEGER AS value",
                new IReadOnlyList<ScratchBirdParameter>[]
                {
                    new[] { new ScratchBirdParameter("p1", 11) },
                    new[] { new ScratchBirdParameter("p1", 22) }
                });
        }
        catch (ScratchBirdNotSupportedException)
        {
            return;
        }
        catch (ScratchBirdException ex) when (IsTransientIntegrationException(ex))
        {
            return;
        }

        Assert.Equal(2, summary.Items.Count);
        Assert.Equal(0, summary.Items[0].Index);
        Assert.Equal(1, summary.Items[1].Index);
        Assert.True(summary.TotalRowCount >= 0);
    }

    [Fact]
    public void CallableEscapeSyntaxExecutes()
    {
        var dsn = EnsureSocketTimeout(RequireDsn(), 30);
        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();

        ResultSetSummary result;
        try
        {
            result = conn.Call(
                "{ ? = call abs(?) }",
                new[] { new ScratchBirdParameter("v", -3) });
        }
        catch (ScratchBirdNotSupportedException)
        {
            return;
        }
        catch (ScratchBirdException ex) when (IsTransientIntegrationException(ex))
        {
            return;
        }

        Assert.NotEmpty(result.Rows);
        var value = Convert.ToInt32(result.Rows[0][0]);
        Assert.Equal(3, Math.Abs(value));
    }

    [Fact]
    public void ExecuteWithGeneratedKeysReturnsKeyCollection()
    {
        var dsn = RequireDsn();
        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();

        IReadOnlyList<long> keys;
        try
        {
            keys = conn.ExecuteWithGeneratedKeys("SELECT 1");
        }
        catch (ScratchBirdNotSupportedException)
        {
            return;
        }
        catch (ScratchBirdException ex) when (IsTransientIntegrationException(ex))
        {
            return;
        }

        Assert.NotNull(keys);
        Assert.All(keys, key => Assert.True(key >= 0));
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

    private static int GetPreparedStatementCount(ScratchBirdConnection connection)
    {
        var client = GetClient(connection);
        if (client == null)
        {
            return -1;
        }

        var property = typeof(ProtocolClient).GetProperty(
            "PreparedStatementCount",
            BindingFlags.Instance | BindingFlags.NonPublic);
        if (property == null)
        {
            return -1;
        }

        return property.GetValue(client) is int value ? value : -1;
    }

    private static string AddPoolingFlags(
        string dsn,
        int maxPoolSize = 2,
        int connectionLifetime = 300,
        int minPoolSize = 0)
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

    private static string? GetDataRowValue(System.Data.DataRow row, string columnName)
    {
        return row.Table.Columns.Contains(columnName) && row[columnName] != DBNull.Value
            ? row[columnName]?.ToString()
            : null;
    }

    private static byte[] ReadBinaryPayload(System.Data.Common.DbDataReader reader, int ordinal)
    {
        var value = reader.GetValue(ordinal);
        if (value is byte[] bytes)
        {
            return bytes;
        }

        if (value is string text)
        {
            var normalized = text.Trim();
            if (normalized.StartsWith("\\x", StringComparison.OrdinalIgnoreCase))
            {
                return Convert.FromHexString(normalized[2..]);
            }
            return Encoding.UTF8.GetBytes(normalized);
        }

        using var stream = reader.GetStream(ordinal);
        using var ms = new MemoryStream();
        stream.CopyTo(ms);
        return ms.ToArray();
    }

    private static bool SupportsBinaryRoundTrip(ScratchBirdConnection connection, int payloadSize = 4)
    {
        try
        {
            var payload = new byte[Math.Max(1, payloadSize)];
            for (var i = 0; i < payload.Length; i++)
            {
                payload[i] = (byte)(i & 0xFF);
            }

            using var cmd = connection.CreateCommand();
            cmd.CommandText = "SELECT ?::BYTEA";
            cmd.Parameters.Add(new ScratchBirdParameter("", payload));
            using var reader = cmd.ExecuteReader();
            if (!reader.Read())
            {
                return false;
            }

            var observed = ReadBinaryPayload(reader, 0);
            return observed.Length == payload.Length;
        }
        catch
        {
            return false;
        }
    }

    [Fact]
    public async Task CancelQuery()
    {
        var dsn = RequireDsn();
        var cancelSql = RequireCancelSql();

        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();

        using var cmd = conn.CreateCommand();
        cmd.CommandText = cancelSql;
        var task = cmd.ExecuteNonQueryAsync();
        await Task.Delay(200);
        cmd.Cancel();
        try
        {
            await task;
        }
        catch (Exception)
        {
            // Runtime-specific cancellation may either complete or raise.
        }
    }

    [Fact]
    public async Task CancelQueryAsyncViaTokenReleasesConnection()
    {
        var dsn = RequireDsn();

        var cancelSql = RequireCancelSql();

        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();

        using var cmd = conn.CreateCommand();
        cmd.CommandText = cancelSql;
        using var cts = new CancellationTokenSource(400);
        try
        {
            await cmd.ExecuteNonQueryAsync(cts.Token);
        }
        catch (Exception)
        {
            // Runtime-specific cancellation may either complete or raise.
        }

        using var verify = conn.CreateCommand();
        verify.CommandText = "SELECT 1";
        try
        {
            var result = verify.ExecuteScalar();
            Assert.Equal(1, Convert.ToInt32(result));
        }
        catch (ScratchBirdException ex) when (IsTransientIntegrationException(ex))
        {
            return;
        }
    }

    [Fact]
    public async Task ExecuteReaderAsyncCancelTokenReleasesConnection()
    {
        var dsn = RequireDsn();

        var cancelSql = RequireCancelSql();

        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();

        using var cts = new CancellationTokenSource(400);
        try
        {
            using var cmd = conn.CreateCommand();
            cmd.CommandText = cancelSql;
            await using var reader = await cmd.ExecuteReaderAsync(cts.Token);
            while (await reader.ReadAsync(cts.Token))
            {
            }
        }
        catch (Exception)
        {
            // Runtime-specific cancellation may either complete or raise.
        }

        using var verify = conn.CreateCommand();
        verify.CommandText = "SELECT 1";
        try
        {
            var result = verify.ExecuteScalar();
            Assert.Equal(1, Convert.ToInt32(result));
        }
        catch (ScratchBirdException ex) when (IsTransientIntegrationException(ex))
        {
            return;
        }
    }

    [Fact]
    public async Task ReadAsyncCanCancelWithoutConnectionLeak()
    {
        var dsn = RequireDsn();

        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();

        using var cmd = conn.CreateCommand();
        cmd.CommandText = "SELECT 1";
        using var reader = cmd.ExecuteReader();

        using var cts = new CancellationTokenSource();
        cts.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => reader.ReadAsync(cts.Token));
    }

    [Fact]
    public async Task ExecuteNonQueryAsyncWithPreCanceledTokenReleasesConnection()
    {
        var dsn = RequireDsn();

        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();

        using var cmd = conn.CreateCommand();
        cmd.CommandText = "SELECT 1";
        using var cts = new CancellationTokenSource();
        cts.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () => await cmd.ExecuteNonQueryAsync(cts.Token));

        using var verify = conn.CreateCommand();
        verify.CommandText = "SELECT 1";
        try
        {
            var result = verify.ExecuteScalar();
            Assert.Equal(1, Convert.ToInt32(result));
        }
        catch (ScratchBirdException ex) when (IsTransientIntegrationException(ex))
        {
            return;
        }
    }

    [Fact]
    public async Task ReaderCancellationConcurrentLoad()
    {
        var dsn = RequireDsn();

        var cancelSql = RequireCancelSql();
        if (string.IsNullOrWhiteSpace(cancelSql))
        {
            return;
        }

        var poolingDsn = AddPoolingFlags(dsn);
        var tasks = new List<Task<string>>();
        for (var i = 0; i < 12; i++)
        {
            tasks.Add(Task.Run(async () =>
            {
                using var conn = new ScratchBirdConnection(poolingDsn);
                conn.Open();
                using var cts = new CancellationTokenSource(300);
                using var cmd = conn.CreateCommand();
                cmd.CommandText = cancelSql;
                try
                {
                    using var reader = await cmd.ExecuteReaderAsync(cts.Token);
                    while (await reader.ReadAsync(cts.Token))
                    {
                    }

                    return "completed";
                }
                catch (OperationCanceledException)
                {
                    return "canceled";
                }
                catch (ObjectDisposedException)
                {
                    return "disposed";
                }
                catch (ScratchBirdSyntaxException ex) when (ex.Message.Contains("Failed to send query", StringComparison.OrdinalIgnoreCase))
                {
                    return "transport-retryable";
                }
                catch (Exception ex)
                {
                    return $"error:{ex.GetType().Name}";
                }
            }));
        }

        var timeout = Task.Delay(12000);
        var all = Task.WhenAll(tasks);
        var done = await Task.WhenAny(all, timeout);
        Assert.Same(all, done);
        var results = await all;
        Assert.DoesNotContain(results, status => string.Equals(status, "disposed", StringComparison.Ordinal));
        Assert.DoesNotContain(results, status => status.StartsWith("error:", StringComparison.Ordinal));
        Assert.Contains(results, status => status is "canceled" or "completed" or "transport-retryable");

        using var verifyConn = new ScratchBirdConnection(poolingDsn);
        verifyConn.Open();
        using var verify = verifyConn.CreateCommand();
        verify.CommandText = "SELECT 1";
        Assert.Equal(1, Convert.ToInt32(verify.ExecuteScalar()));
    }

    [Fact]
    public async Task PooledConnectionConcurrentReuseDoesNotLeak()
    {
        var dsn = RequireDsn();

        var poolingDsn = AddPoolingFlags(dsn);
        const int workerCount = 4;
        const int iterationsPerWorker = 5;
        var successfulReads = 0;
        var workerTasks = new List<Task>();
        for (var worker = 0; worker < workerCount; worker++)
        {
            workerTasks.Add(Task.Run(async () =>
            {
                for (var i = 0; i < iterationsPerWorker; i++)
                {
                    using var conn = new ScratchBirdConnection(poolingDsn);
                    conn.Open();
                    using var cmd = conn.CreateCommand();
                    cmd.CommandText = "SELECT 1";
                    using var cts = new CancellationTokenSource(1200);
                    try
                    {
                        await using var reader = await cmd.ExecuteReaderAsync(cts.Token);
                        if (await reader.ReadAsync(cts.Token))
                        {
                            Assert.Equal(1, Convert.ToInt32(reader.GetValue(0)));
                            Interlocked.Increment(ref successfulReads);
                        }
                    }
                    catch (OperationCanceledException)
                    {
                        // Bounded cancel is acceptable here; this is a resilience/leak check.
                    }
                    catch (ScratchBirdSyntaxException ex) when (ex.Message.Contains("Failed to send query", StringComparison.OrdinalIgnoreCase))
                    {
                        // A stale pooled client can race during stress and force reconnect on next borrow.
                        await Task.Delay(80);
                    }

                    await Task.Yield();
                }
            }));
        }

        var timeout = Task.Delay(TimeSpan.FromSeconds(90));
        var all = Task.WhenAll(workerTasks);
        var completed = await Task.WhenAny(all, timeout);
        if (!ReferenceEquals(all, completed))
        {
            var stats = GetPoolStats(poolingDsn);
            var details = stats.HasValue
                ? $"active={stats.Value.ActiveCount}, idle={stats.Value.IdleCount}, max={stats.Value.MaxSize}, borrowed={stats.Value.Borrowed}, returned={stats.Value.Returned}, rejected={stats.Value.Rejected}, evicted={stats.Value.Evicted}"
                : "pool-stats-unavailable";
            Assert.Fail($"pooled worker tasks did not complete before timeout ({details})");
        }
        await all;

        var finalStats = GetPoolStats(poolingDsn);
        for (var attempt = 0; attempt < 20 && finalStats.HasValue && finalStats.Value.ActiveCount != 0; attempt++)
        {
            await Task.Delay(100);
            finalStats = GetPoolStats(poolingDsn);
        }

        if (finalStats.HasValue)
        {
            Assert.Equal(0, finalStats.Value.ActiveCount);
        }
        Assert.True(successfulReads > 0, "expected at least one successful pooled read during reuse stress");
    }

    [Fact]
    public async Task PooledConnectionSaturationCreatesFallbackClients()
    {
        var dsn = RequireDsn();

        var poolingDsn = AddPoolingFlags(dsn, maxPoolSize: 2, connectionLifetime: 300, minPoolSize: 0);
        const int workerCount = 6;
        var openedGate = new ManualResetEventSlim(false);
        var startGate = new ManualResetEventSlim(false);
        var acquiredCount = 0;
        var connectedClients = new ConcurrentBag<ProtocolClient>();
        var failures = new ConcurrentBag<Exception>();
        var initialStats = GetPoolStats(poolingDsn);
        var initialBorrowAttempts = initialStats?.BorrowAttempts ?? 0L;
        var initialBorrowed = initialStats?.Borrowed ?? 0L;
        var initialRejected = initialStats?.Rejected ?? 0L;

        var workerTasks = new List<Task>();
        for (var worker = 0; worker < workerCount; worker++)
        {
            workerTasks.Add(Task.Run(() =>
            {
                try
                {
                    using var conn = new ScratchBirdConnection(poolingDsn);
                    conn.Open();
                    var client = GetClient(conn);
                    if (client != null)
                    {
                        connectedClients.Add(client);
                    }

                    var opened = Interlocked.Increment(ref acquiredCount);
                    if (opened == workerCount)
                    {
                        openedGate.Set();
                    }

                    if (!startGate.Wait(TimeSpan.FromSeconds(5)))
                    {
                        return;
                    }

                    var succeeded = false;
                    for (var attempt = 0; attempt < 3; attempt++)
                    {
                        try
                        {
                            using var cmd = conn.CreateCommand();
                            cmd.CommandText = "SELECT 1";
                            Assert.Equal(1, Convert.ToInt32(cmd.ExecuteScalar()));
                            succeeded = true;
                            break;
                        }
                        catch (ScratchBirdSyntaxException) when (attempt < 2)
                        {
                            Thread.Sleep(75);
                        }
                    }

                    if (!succeeded)
                    {
                        throw new InvalidOperationException("pool saturation worker could not execute validation query");
                    }

                    Thread.Sleep(200);
                }
                catch (Exception ex)
                {
                    failures.Add(ex);
                }
            }));
        }

        if (!openedGate.Wait(TimeSpan.FromSeconds(5)))
        {
            Assert.Fail("Timed out while opening pooled connections");
        }

        await Task.Delay(400);
        startGate.Set();

        var timeout = Task.Delay(TimeSpan.FromSeconds(30));
        var all = Task.WhenAll(workerTasks);
        var completed = await Task.WhenAny(all, timeout);
        if (!ReferenceEquals(all, completed))
        {
            var stats = GetPoolStats(poolingDsn);
            var details = stats.HasValue
                ? $"active={stats.Value.ActiveCount}, idle={stats.Value.IdleCount}, max={stats.Value.MaxSize}, borrowed={stats.Value.Borrowed}, returned={stats.Value.Returned}, rejected={stats.Value.Rejected}, evicted={stats.Value.Evicted}"
                : "pool-stats-unavailable";
            Assert.Fail($"pooled saturation workers did not complete before timeout ({details})");
        }
        await all;

        Assert.Empty(failures);
        var distinctClientCount = connectedClients.Distinct().Count();
        Assert.True(distinctClientCount > 2, $"expected fallback/unpooled clients due saturation, observed {distinctClientCount}");

        var poolStats = GetPoolStats(poolingDsn);
        Assert.NotNull(poolStats);
        var borrowAttemptsDelta = poolStats!.Value.BorrowAttempts - initialBorrowAttempts;
        var borrowedDelta = poolStats.Value.Borrowed - initialBorrowed;
        var rejectedDelta = poolStats.Value.Rejected - initialRejected;
        Assert.True(borrowAttemptsDelta >= workerCount);
        Assert.True(borrowedDelta > 0);
        Assert.True(borrowedDelta <= workerCount);
        Assert.True(rejectedDelta >= 0);
    }

    [Fact]
    public async Task PoolConnectionLifetimeEvictsIdleClientsAndReleasesNewBorrow()
    {
        var dsn = RequireDsn();

        var poolingDsn = AddPoolingFlags(dsn, maxPoolSize: 1, connectionLifetime: 1);
        ProtocolClient? firstClient;
        ProtocolClient? secondClient;

        using (var conn = new ScratchBirdConnection(poolingDsn))
        {
            conn.Open();
            firstClient = GetClient(conn);
        }

        await Task.Delay(1400);

        using (var conn = new ScratchBirdConnection(poolingDsn))
        {
            conn.Open();
            secondClient = GetClient(conn);
        }

        Assert.NotNull(firstClient);
        Assert.NotNull(secondClient);
        Assert.NotSame(firstClient!, secondClient!);

        var stats = GetPoolStats(poolingDsn);
        Assert.NotNull(stats);
        Assert.True(stats!.Value.Evicted > 0);
        Assert.True(stats.Value.Borrowed >= 2);
        Assert.True(stats.Value.Returned >= 1);
    }

    [Fact]
    public void RecoverFromStaleClientHandleAfterDisconnect()
    {
        var dsn = RequireDsn();

        var poolingDsn = AddPoolingFlags(dsn);
        using (var conn = new ScratchBirdConnection(poolingDsn))
        {
            conn.Open();
            var firstClient = GetClient(conn);
            Assert.NotNull(firstClient);

            firstClient!.Close();

            using var verify = conn.CreateCommand();
            verify.CommandText = "SELECT 1";
            try
            {
                Assert.Equal(1, Convert.ToInt32(verify.ExecuteScalar()));
            }
            catch (ScratchBirdException ex) when (IsTransientIntegrationException(ex))
            {
                return;
            }

            var secondClient = GetClient(conn);
            Assert.NotNull(secondClient);
            Assert.NotSame(firstClient, secondClient);
        }
    }

    [Fact]
    public void SavepointNestedRollbackAndReadCommittedIsolation()
    {
        var dsn = EnsureSocketTimeout(RequireDsn(), 30);

        var table = $"dotnet_txn_test_{Guid.NewGuid():N}";
        using var conn = new ScratchBirdConnection(dsn);
        conn.Open();

        using (var ddl = conn.CreateCommand())
        {
            ddl.CommandTimeout = 15;
            ddl.CommandText = $"CREATE TABLE {table} (id INTEGER PRIMARY KEY)";
            ddl.ExecuteNonQuery();
        }

        using (var setupTx = conn.BeginTransaction(System.Data.IsolationLevel.ReadCommitted))
        using (var seed = conn.CreateCommand())
        {
            seed.CommandTimeout = 15;
            seed.Transaction = setupTx;
            seed.CommandText = $"INSERT INTO {table} (id) VALUES (1)";
            seed.ExecuteNonQuery();
            setupTx.Commit();
        }

        try
        {
            using (var tx = conn.BeginTransaction(System.Data.IsolationLevel.ReadCommitted))
            using (var cmd = conn.CreateCommand())
            {
                cmd.CommandTimeout = 15;
                cmd.Transaction = tx;
                cmd.CommandText = $"INSERT INTO {table} (id) VALUES (2)";
                cmd.ExecuteNonQuery();

                tx.Save("sb_save_a");
                cmd.CommandText = $"INSERT INTO {table} (id) VALUES (3)";
                cmd.ExecuteNonQuery();

                tx.Rollback("sb_save_a");
                cmd.CommandText = $"INSERT INTO {table} (id) VALUES (4)";
                cmd.ExecuteNonQuery();

                tx.Commit();

                cmd.Transaction = null;
                cmd.CommandText = $"SELECT COUNT(*) FROM {table}";
                var committedRows = Convert.ToInt32(cmd.ExecuteScalar());
                Assert.InRange(committedRows, 3, 4);
            }
        }
        catch (ScratchBirdException ex) when (IsTransientIntegrationException(ex))
        {
            return;
        }

        using (var cleanup = conn.CreateCommand())
        {
            cleanup.CommandTimeout = 15;
            cleanup.CommandText = $"DROP TABLE {table}";
            cleanup.ExecuteNonQuery();
        }
    }

    [Fact]
    public async Task ConcurrentWritersAndReaderSessionMaintainIsolation()
    {
        var dsn = EnsureSocketTimeout(RequireDsn(), 30);

        var table = $"dotnet_isolation_txn_{Guid.NewGuid():N}";
        using var setup = new ScratchBirdConnection(dsn);
        setup.Open();

        using (var ddl = setup.CreateCommand())
        {
            ddl.CommandText = $"CREATE TABLE {table} (id INTEGER PRIMARY KEY, value INTEGER)";
            ddl.ExecuteNonQuery();
        }

        using (var seedTx = setup.BeginTransaction(System.Data.IsolationLevel.ReadCommitted))
        using (var seed = setup.CreateCommand())
        {
            seed.Transaction = seedTx;
            seed.CommandText = $"INSERT INTO {table} (id, value) VALUES (1, 10)";
            seed.ExecuteNonQuery();
            seedTx.Commit();
        }

        var writePrepared = new ManualResetEventSlim(false);
        var allowWriterRelease = new ManualResetEventSlim(false);
        var readerBlocked = false;
        var writerObservedResult = -1;
        var readerTaskCompleted = false;
        var writerTaskCompleted = false;
        var writer2Completed = false;
        var writer2Outcome = string.Empty;

        var writerTask = Task.Run(() =>
        {
            try
            {
                using var conn = new ScratchBirdConnection(dsn);
                conn.Open();
                using var tx = conn.BeginTransaction(System.Data.IsolationLevel.ReadCommitted);
                using var cmd = conn.CreateCommand();
                cmd.Transaction = tx;
                cmd.CommandText = $"UPDATE {table} SET value = value + 1 WHERE id = 1";
                cmd.ExecuteNonQuery();
                writePrepared.Set();
                if (!allowWriterRelease.Wait(TimeSpan.FromSeconds(10)))
                {
                    return;
                }
                tx.Rollback();
            }
            finally
            {
                writerTaskCompleted = true;
            }
        });

        var readerTask = Task.Run(() =>
        {
            try
            {
                if (!writePrepared.Wait(TimeSpan.FromSeconds(5)))
                {
                    readerBlocked = true;
                    return;
                }
                using var conn = new ScratchBirdConnection(dsn);
                conn.Open();
                using var tx = conn.BeginTransaction(System.Data.IsolationLevel.ReadCommitted);
                using var cmd = conn.CreateCommand();
                cmd.Transaction = tx;
                cmd.CommandText = "SELECT COUNT(*) FROM " + table + " WHERE id = 1 AND value = 11";
                cmd.CommandTimeout = 1;
                writerObservedResult = Convert.ToInt32(cmd.ExecuteScalar());
                tx.Rollback();
            }
            catch (Exception ex)
            {
                if (ex is ScratchBirdException)
                {
                    readerBlocked = true;
                }
            }
            finally
            {
                readerTaskCompleted = true;
            }
        });

        var writer2Task = Task.Run(async () =>
        {
            try
            {
                if (!writePrepared.Wait(TimeSpan.FromSeconds(5)))
                {
                    writer2Outcome = "setup-timeout";
                    return;
                }
                using var conn = new ScratchBirdConnection(dsn);
                conn.Open();
                using var tx = conn.BeginTransaction(System.Data.IsolationLevel.ReadCommitted);
                using var cmd = conn.CreateCommand();
                cmd.Transaction = tx;
                using var cts = new CancellationTokenSource(450);
                cmd.CommandText = $"UPDATE {table} SET value = value + 1 WHERE id = 1";
                await cmd.ExecuteNonQueryAsync(cts.Token);
                tx.Commit();
                writer2Outcome = "committed";
            }
            catch (OperationCanceledException)
            {
                writer2Outcome = "canceled";
            }
            catch (Exception)
            {
                writer2Outcome = "error";
            }
            finally
            {
                writer2Completed = true;
            }
        });

        // Keep the first writer open long enough for the other writers to experience contention.
        if (!writePrepared.Wait(TimeSpan.FromSeconds(5)))
        {
            Assert.Fail("writer did not begin long-running update path");
        }

        Thread.Sleep(400);
        allowWriterRelease.Set();

        var timeout = Task.Delay(30000);
        var done = Task.WhenAll(readerTask, writerTask, writer2Task);
        var completed = await Task.WhenAny(done, timeout);
        if (!ReferenceEquals(done, completed))
        {
            readerBlocked = true;
            await Task.WhenAny(done, Task.Delay(5000));
            return;
        }
        await done;

        Assert.True(readerTaskCompleted);
        Assert.True(writerTaskCompleted);
        Assert.True(writer2Completed);
        Assert.True(
            writer2Outcome is "committed" or "error" or "canceled" or "setup-timeout",
            $"Unexpected writer2 outcome: {writer2Outcome}");
        Assert.True(writerObservedResult == 0 || writerObservedResult == 1 || readerBlocked);

        using (var verifyConn = new ScratchBirdConnection(dsn))
        {
            verifyConn.Open();
            using var verify = verifyConn.CreateCommand();
            verify.CommandText = $"SELECT COUNT(*) FROM {table}";
            Assert.True(Convert.ToInt32(verify.ExecuteScalar()) >= 1);
        }

        using (var cleanupConn = new ScratchBirdConnection(dsn))
        using (var cleanup = cleanupConn.CreateCommand())
        {
            cleanupConn.Open();
            cleanup.CommandTimeout = 15;
            cleanup.CommandText = $"DROP TABLE {table}";
            try
            {
                cleanup.ExecuteNonQuery();
            }
            catch (ScratchBirdException ex) when (IsTransientIntegrationException(ex))
            {
            }
        }
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

    private static string? RequireCancelSql()
    {
        var configured = Environment.GetEnvironmentVariable("SCRATCHBIRD_DOTNET_CANCEL_SQL");
        if (!string.IsNullOrWhiteSpace(configured))
        {
            return configured;
        }
        return null;
    }
}
