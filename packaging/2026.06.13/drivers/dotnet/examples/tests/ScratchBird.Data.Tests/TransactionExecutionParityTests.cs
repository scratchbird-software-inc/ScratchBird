// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Data;
using System.IO;
using System.Reflection;
using System.Text;
using ScratchBird.Data;
using Xunit;

namespace ScratchBird.Data.Tests;

public class TransactionExecutionParityTests
{
    [Fact]
    public void BeginTransaction_ThrowsWhenConnectionAlreadyHasActiveTransaction()
    {
        using var connection = CreateOpenConnection();
        var activeTransaction = new ScratchBirdTransaction(connection, IsolationLevel.ReadCommitted);
        SetPrivateField(connection, "_activeTransaction", activeTransaction);

        var ex = Assert.Throws<InvalidOperationException>(() => connection.BeginTransaction());
        Assert.Contains("active transaction", ex.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void BeginTransactionWithOptions_ThrowsWhenOptionsNull()
    {
        using var connection = CreateOpenConnection();
        Assert.Throws<ArgumentNullException>(() => connection.BeginTransaction(options: null!));
    }

    [Fact]
    public void BeginTransaction_RestartsImplicitBoundaryAndSendsTxnBegin()
    {
        var stream = new ProtocolCaptureStream(BuildReadyMessage(1, 41));
        using var connection = CreateOpenConnectionWithHealthyClient(stream);
        var client = (ProtocolClient)GetPrivateField(connection, "_client")!;
        SetPrivateField(client, "_runtimeTxnActive", true);
        SetPrivateField(client, "_txnId", 0UL);

        using var transaction = connection.BeginTransaction(System.Data.IsolationLevel.ReadCommitted);

        Assert.NotNull(transaction);
        Assert.True(connection.HasActiveTransaction);
        var messages = ParseWrittenMessages(stream.WrittenBytes);
        Assert.Single(messages);
        Assert.Equal(MessageType.TXN_BEGIN, (MessageType)messages[0].Header.Type);
    }

    [Fact]
    public void BeginTransactionWithOptions_AllowsNonDefaultRestartOptions()
    {
        var stream = new ProtocolCaptureStream(BuildReadyMessage(1, 42));
        using var connection = CreateOpenConnectionWithHealthyClient(stream);
        var client = (ProtocolClient)GetPrivateField(connection, "_client")!;
        SetPrivateField(client, "_runtimeTxnActive", true);
        SetPrivateField(client, "_txnId", 0UL);

        using var transaction = connection.BeginTransaction(new ScratchBirdTransactionOptions
        {
            IsolationLevel = IsolationLevel.Serializable
        });
        Assert.NotNull(transaction);
        var messages = ParseWrittenMessages(stream.WrittenBytes);
        Assert.Single(messages);
        Assert.Equal(MessageType.TXN_BEGIN, (MessageType)messages[0].Header.Type);
    }

    [Fact]
    public void EnsureConnectedClient_ClearsActiveTransactionBeforeReconnectFailure()
    {
        using var connection = new ScratchBirdConnection(
            "Host=127.0.0.1;Port=1;Database=main;Username=sb_admin;Password=SbAdmin_Compat1!;Pooling=false;SslMode=disable;AllowInsecure=true;Connect Timeout=1;Socket Timeout=1");
        SetPrivateField(connection, "_state", ConnectionState.Open);
        SetPrivateField(connection, "_client", new ProtocolClient());

        var activeTransaction = new ScratchBirdTransaction(connection, IsolationLevel.ReadCommitted);
        SetPrivateField(connection, "_activeTransaction", activeTransaction);

        var ex = Assert.Throws<TargetInvocationException>(() => InvokePrivateMethod(connection, "EnsureConnectedClient"));
        Assert.NotNull(ex.InnerException);
        Assert.True(
            ex.InnerException is ScratchBirdException || ex.InnerException is AggregateException,
            $"Unexpected reconnect failure type: {ex.InnerException.GetType().FullName}");
        Assert.Null(GetPrivateField(connection, "_activeTransaction"));
        Assert.Null(GetPrivateField(connection, "_client"));
    }

    [Fact]
    public void ExecuteNonQuery_ThrowsWhenCommandTextMissing()
    {
        using var connection = CreateOpenConnection();
        using var command = connection.CreateCommand();
        command.CommandText = "   ";

        var ex = Assert.Throws<InvalidOperationException>(() => command.ExecuteNonQuery());
        Assert.Contains("CommandText must be set", ex.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ExecuteNonQuery_ThrowsWhenConnectionHasActiveTransactionAndCommandTransactionIsMissing()
    {
        using var connection = CreateOpenConnection();
        var activeTransaction = new ScratchBirdTransaction(connection, IsolationLevel.ReadCommitted);
        SetPrivateField(connection, "_activeTransaction", activeTransaction);

        using var command = connection.CreateCommand();
        command.CommandText = "SELECT 1";

        var ex = Assert.Throws<InvalidOperationException>(() => command.ExecuteNonQuery());
        Assert.Contains("requires an explicit Transaction", ex.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ExecuteNonQuery_ThrowsWhenTransactionBelongsToDifferentConnection()
    {
        using var connection = CreateOpenConnection();
        using var otherConnection = CreateOpenConnection();
        var transaction = new ScratchBirdTransaction(otherConnection, IsolationLevel.ReadCommitted);

        using var command = connection.CreateCommand();
        command.CommandText = "SELECT 1";
        command.Transaction = transaction;

        var ex = Assert.Throws<InvalidOperationException>(() => command.ExecuteNonQuery());
        Assert.Contains("not associated", ex.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void ExecuteNonQuery_ThrowsWhenTransactionAlreadyCompleted()
    {
        using var connection = CreateOpenConnection();
        var transaction = new ScratchBirdTransaction(connection, IsolationLevel.ReadCommitted);
        SetPrivateField(connection, "_activeTransaction", transaction);
        SetPrivateField(transaction, "_completed", true);

        using var command = connection.CreateCommand();
        command.CommandText = "SELECT 1";
        command.Transaction = transaction;

        var ex = Assert.Throws<InvalidOperationException>(() => command.ExecuteNonQuery());
        Assert.Contains("already completed", ex.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Savepoint_ThrowsWhenTransactionIsNotRegisteredAsActive()
    {
        using var connection = CreateOpenConnection();
        using var transaction = new ScratchBirdTransaction(connection, IsolationLevel.ReadCommitted);

        var ex = Assert.Throws<InvalidOperationException>(() => transaction.Save("s1"));
        Assert.Contains("not active", ex.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void CommandTimeout_ThrowsForNegativeValues()
    {
        using var command = new ScratchBirdCommand();
        Assert.Throws<ArgumentOutOfRangeException>(() => command.CommandTimeout = -1);
    }

    [Fact]
    public void CommandType_AllowsStoredProcedureAndRejectsTableDirect()
    {
        using var command = new ScratchBirdCommand();
        command.CommandType = CommandType.StoredProcedure;
        Assert.Equal(CommandType.StoredProcedure, command.CommandType);
        Assert.Throws<NotSupportedException>(() => command.CommandType = CommandType.TableDirect);
    }

    [Fact]
    public void Prepare_ForStoredProcedureBuildsCallableSql()
    {
        using var connection = CreateOpenConnection();
        using var command = connection.CreateCommand();
        command.CommandType = CommandType.StoredProcedure;
        command.CommandText = "sys.echo_value";
        command.Parameters.Add(new ScratchBirdParameter("v1", 42));
        command.Parameters.Add(new ScratchBirdParameter("v2", "ok"));

        command.Prepare();

        var prepared = (object?)GetPrivateField(command, "_preparedQuery");
        Assert.NotNull(prepared);
        var sql = (string?)prepared!.GetType().GetProperty("Sql")?.GetValue(prepared);
        Assert.Equal("CALL \"sys\".\"echo_value\"($1, $2)", sql);
    }

    [Fact]
    public void Prepare_ForStoredProcedureExcludesReturnValueFromCallableArgs()
    {
        using var connection = CreateOpenConnection();
        using var command = connection.CreateCommand();
        command.CommandType = CommandType.StoredProcedure;
        command.CommandText = "sys.echo_value";
        command.Parameters.Add(new ScratchBirdParameter("ret", null) { Direction = ParameterDirection.ReturnValue });
        command.Parameters.Add(new ScratchBirdParameter("in1", 11) { Direction = ParameterDirection.Input });
        command.Parameters.Add(new ScratchBirdParameter("out1", null) { Direction = ParameterDirection.Output });
        command.Parameters.Add(new ScratchBirdParameter("inout1", 7) { Direction = ParameterDirection.InputOutput });

        command.Prepare();

        var prepared = (object?)GetPrivateField(command, "_preparedQuery");
        Assert.NotNull(prepared);
        var sql = (string?)prepared!.GetType().GetProperty("Sql")?.GetValue(prepared);
        Assert.Equal("CALL \"sys\".\"echo_value\"($1, $2, $3)", sql);

        var preparedParams = (IReadOnlyList<ScratchBirdParameter>?)prepared.GetType().GetProperty("Parameters")?.GetValue(prepared);
        Assert.NotNull(preparedParams);
        Assert.Equal(3, preparedParams!.Count);
        Assert.DoesNotContain(preparedParams, parameter => parameter.Direction == ParameterDirection.ReturnValue);
    }

    [Fact]
    public void ExecuteNonQuery_RejectsOutputDirectionForCommandTypeText()
    {
        using var connection = CreateOpenConnection();
        using var command = connection.CreateCommand();
        command.CommandType = CommandType.Text;
        command.CommandText = "SELECT 1";
        command.Parameters.Add(new ScratchBirdParameter("p1", null) { Direction = ParameterDirection.Output });

        var ex = Assert.Throws<NotSupportedException>(() => command.ExecuteNonQuery());
        Assert.Contains("CommandType.StoredProcedure", ex.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void ApplyCallableOutputValues_MapsReturnOutputAndInputOutput()
    {
        var parameters = new List<ScratchBirdParameter>
        {
            new("ret", null) { Direction = ParameterDirection.ReturnValue },
            new("in1", 5) { Direction = ParameterDirection.Input },
            new("out1", null) { Direction = ParameterDirection.Output },
            new("inout1", 7) { Direction = ParameterDirection.InputOutput }
        };

        var method = typeof(ScratchBirdCommand).GetMethod(
            "ApplyCallableOutputValues",
            BindingFlags.NonPublic | BindingFlags.Static);
        Assert.NotNull(method);

        method!.Invoke(null, new object?[] { parameters, new object?[] { 99, "done", 123 } });

        Assert.Equal(99, parameters[0].Value);
        Assert.Equal("done", parameters[2].Value);
        Assert.Equal(123, parameters[3].Value);
    }

    [Fact]
    public void ApplyCallableOutputValues_SetsDbNullForMissingOutputColumns()
    {
        var parameters = new List<ScratchBirdParameter>
        {
            new("ret", null) { Direction = ParameterDirection.ReturnValue },
            new("in1", 5) { Direction = ParameterDirection.Input },
            new("out1", null) { Direction = ParameterDirection.Output },
            new("inout1", 7) { Direction = ParameterDirection.InputOutput }
        };

        var method = typeof(ScratchBirdCommand).GetMethod(
            "ApplyCallableOutputValues",
            BindingFlags.NonPublic | BindingFlags.Static);
        Assert.NotNull(method);

        method!.Invoke(null, new object?[] { parameters, new object?[] { 42 } });

        Assert.Equal(42, parameters[0].Value);
        Assert.Equal(DBNull.Value, parameters[2].Value);
        Assert.Equal(7, parameters[3].Value);
    }

    [Fact]
    public void NativeCallableSql_RewritesJdbcEscapeSyntax()
    {
        using var connection = new ScratchBirdConnection();
        var sql = connection.NativeCallableSql(
            "{ ? = call abs(?) }",
            new[]
            {
                new ScratchBirdParameter("v", -7)
            });
        Assert.Equal("select abs($1) as return_value", sql);
    }

    [Fact]
    public void ExecuteBatch_ThrowsWhenBatchParametersMissing()
    {
        using var connection = CreateOpenConnection();
        var ex = Assert.Throws<ArgumentException>(() =>
            connection.ExecuteBatch("SELECT 1", new List<IReadOnlyList<ScratchBirdParameter>>()));
        Assert.Contains("batch parameters are required", ex.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Theory]
    [InlineData(IsolationLevel.Snapshot)]
    [InlineData(IsolationLevel.Chaos)]
    public void IsolationLevelSnapshotAndChaosMapToSerializable(IsolationLevel input)
    {
        var method = typeof(ProtocolClient).GetMethod(
            "MapIsolationLevel",
            BindingFlags.NonPublic | BindingFlags.Static);
        Assert.NotNull(method);
        var mapped = (byte)method!.Invoke(null, new object[] { input })!;
        Assert.Equal(ProtocolConstants.IsolationSerializable, mapped);
    }

    [Fact]
    public void CreateTxnBeginPayload_DefaultOptionsSetsReadCommittedIsolationOnly()
    {
        var payload = ProtocolClient.CreateTxnBeginPayload(new ScratchBirdTransactionOptions());

        Assert.Equal(12, payload.Length);
        Assert.Equal(ProtocolConstants.TxnFlagHasIsolation, BinaryPrimitives.ReadUInt16LittleEndian(payload.AsSpan(0, 2)));
        Assert.Equal(0, payload[3]); // autocommit
        Assert.Equal(ProtocolConstants.IsolationReadCommitted, payload[4]);
        Assert.Equal(0, payload[5]); // access
        Assert.Equal(0, payload[6]); // deferrable
        Assert.Equal(0, payload[7]); // wait
        Assert.Equal(0u, BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(8, 4)));
    }

    [Fact]
    public void CreateTxnBeginPayload_WithOptionalsSetsFlagsAndFields()
    {
        var payload = ProtocolClient.CreateTxnBeginPayload(new ScratchBirdTransactionOptions
        {
            IsolationLevel = IsolationLevel.Serializable,
            AccessMode = ScratchBirdTransactionAccessMode.ReadOnly,
            Deferrable = true,
            Wait = false,
            TimeoutMs = 1500,
            AutoCommit = true
        });

        var expectedFlags =
            ProtocolConstants.TxnFlagHasIsolation |
            ProtocolConstants.TxnFlagHasAccess |
            ProtocolConstants.TxnFlagHasDeferrable |
            ProtocolConstants.TxnFlagHasWait |
            ProtocolConstants.TxnFlagHasTimeout |
            ProtocolConstants.TxnFlagHasAutocommit;

        Assert.Equal(expectedFlags, BinaryPrimitives.ReadUInt16LittleEndian(payload.AsSpan(0, 2)));
        Assert.Equal(1, payload[3]); // autocommit true
        Assert.Equal(ProtocolConstants.IsolationSerializable, payload[4]);
        Assert.Equal((byte)ScratchBirdTransactionAccessMode.ReadOnly, payload[5]);
        Assert.Equal(1, payload[6]); // deferrable true
        Assert.Equal(0, payload[7]); // wait false
        Assert.Equal(1500u, BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(8, 4)));
    }

    [Fact]
    public void CreateTxnBeginPayload_WithReadCommittedModeExpandsPayload()
    {
        var payload = ProtocolClient.CreateTxnBeginPayload(new ScratchBirdTransactionOptions
        {
            ReadCommittedMode = ScratchBirdReadCommittedMode.ReadConsistency,
            TimeoutMs = 250
        });

        var expectedFlags =
            ProtocolConstants.TxnFlagHasIsolation |
            ProtocolConstants.TxnFlagHasTimeout |
            ProtocolConstants.TxnFlagHasReadCommittedMode;

        Assert.Equal(16, payload.Length);
        Assert.Equal(expectedFlags, BinaryPrimitives.ReadUInt16LittleEndian(payload.AsSpan(0, 2)));
        Assert.Equal(ProtocolConstants.IsolationReadCommitted, payload[4]);
        Assert.Equal(250u, BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(8, 4)));
        Assert.Equal(ProtocolConstants.ReadCommittedModeReadConsistency, payload[12]);
    }

    [Fact]
    public void CreateTxnBeginPayload_ReadCommittedModeRejectsSnapshotAliases()
    {
        var ex = Assert.Throws<ScratchBirdNotSupportedException>(() =>
            ProtocolClient.CreateTxnBeginPayload(new ScratchBirdTransactionOptions
            {
                IsolationLevel = IsolationLevel.Serializable,
                ReadCommittedMode = ScratchBirdReadCommittedMode.ReadConsistency
            }));
        Assert.Equal("0A000", ex.SqlState);
        Assert.Contains("READ COMMITTED isolation alias", ex.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void CreateTxnBeginPayload_ThrowsWhenTimeoutIsNegative()
    {
        var ex = Assert.Throws<ArgumentOutOfRangeException>(() =>
            ProtocolClient.CreateTxnBeginPayload(new ScratchBirdTransactionOptions { TimeoutMs = -1 }));
        Assert.Contains("TimeoutMs", ex.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void PreparedTransactionHelpersEmitCanonicalControlSql()
    {
        var stream = new ProtocolCaptureStream(
            BuildReadyMessage(1, 11),
            BuildReadyMessage(2, 12),
            BuildReadyMessage(3, 0));
        using var connection = CreateOpenConnectionWithHealthyClient(stream);

        connection.PrepareTransaction("gid-1");
        connection.CommitPrepared("gid-1");
        connection.RollbackPrepared("gid'2");

        var messages = ParseWrittenMessages(stream.WrittenBytes);
        var sql = new List<string>();
        foreach (var message in messages)
        {
            if ((MessageType)message.Header.Type == MessageType.QUERY)
            {
                sql.Add(ParseQuerySql(message.Payload));
            }
        }

        Assert.Equal(
            new[]
            {
                "PREPARE TRANSACTION 'gid-1'",
                "COMMIT PREPARED 'gid-1'",
                "ROLLBACK PREPARED 'gid''2'"
            },
            sql);
    }

    [Fact]
    public void BuildPreparedTransactionSql_RejectsEmptyGid()
    {
        var ex = Assert.Throws<ScratchBirdSyntaxException>(() =>
            ScratchBirdConnection.BuildPreparedTransactionSql("PREPARE TRANSACTION", "   "));
        Assert.Equal("42601", ex.SqlState);
    }

    [Fact]
    public void DormantHelpersExposeTheExplicitNativeTokenFlow()
    {
        using var connection = CreateOpenConnection();

        Assert.True(connection.SupportsPreparedTransactions());
        Assert.True(connection.SupportsDormantReattach());
    }

    [Fact]
    public void DetachToDormant_ReturnsEngineIssuedIdentifiers()
    {
        var stream = new ProtocolCaptureStream(
            BuildParameterStatusMessage(1, "dormant_id", "00112233-4455-6677-8899-aabbccddeeff"),
            BuildParameterStatusMessage(2, "dormant_reattach_token", "ffeeddcc-bbaa-9988-7766-554433221100"),
            BuildReadyMessage(3, 0));
        using var connection = CreateOpenConnectionWithHealthyClient(stream);

        var detached = connection.DetachToDormant();

        Assert.Equal("00112233-4455-6677-8899-aabbccddeeff", detached.DormantId);
        Assert.Equal("ffeeddcc-bbaa-9988-7766-554433221100", detached.ReattachToken);

        var messages = ParseWrittenMessages(stream.WrittenBytes);
        Assert.Single(messages);
        Assert.Equal(MessageType.ATTACH_DETACH, (MessageType)messages[0].Header.Type);
    }

    [Fact]
    public void DetachToDormant_RejectsMissingIdentifiers()
    {
        var stream = new ProtocolCaptureStream(BuildReadyMessage(1, 0));
        using var connection = CreateOpenConnectionWithHealthyClient(stream);

        var ex = Assert.Throws<ScratchBirdConnectionException>(() => connection.DetachToDormant());
        Assert.Equal("08006", ex.SqlState);
    }

    [Fact]
    public void ProtocolHandshake_IncludesDormantStartupParams()
    {
        var stream = new ProtocolCaptureStream(BuildReadyMessage(1, 0));
        var client = new ProtocolClient();
        SetPrivateField(client, "_connected", true);
        SetPrivateField(client, "_stream", stream);

        var config = new ScratchBirdConfig
        {
            Database = "main",
            Username = "sb_admin",
            ConnectClientFlags = 0x0100,
            DormantId = "00112233-4455-6677-8899-aabbccddeeff",
            DormantReattachToken = "ffeeddcc-bbaa-9988-7766-554433221100",
        };

        InvokePrivateMethod(client, "Handshake", config);

        var messages = ParseWrittenMessages(stream.WrittenBytes);
        Assert.Single(messages);
        Assert.Equal(MessageType.STARTUP, (MessageType)messages[0].Header.Type);
        var startupText = Encoding.UTF8.GetString(messages[0].Payload);
        Assert.Contains("dormant_id", startupText, StringComparison.Ordinal);
        Assert.Contains("dormant_reattach_token", startupText, StringComparison.Ordinal);
        Assert.Contains("00112233-4455-6677-8899-aabbccddeeff", startupText, StringComparison.Ordinal);
        Assert.Contains("ffeeddcc-bbaa-9988-7766-554433221100", startupText, StringComparison.Ordinal);
    }

    [Fact]
    public void ReconnectWithDormantParams_RestoresConfigAfterReconnectFailure()
    {
        using var connection = new ScratchBirdConnection(
            "Host=127.0.0.1;Port=1;Database=main;Username=sb_admin;Password=SbAdmin_Compat1!;Pooling=false;SslMode=disable;AllowInsecure=true;Connect Timeout=1;Socket Timeout=1;Schema=analytics.dev");

        var ex = Assert.Throws<TargetInvocationException>(() => InvokePrivateMethod(
            connection,
            "ReconnectWithDormantParams",
            "00112233-4455-6677-8899-aabbccddeeff",
            "ffeeddcc-bbaa-9988-7766-554433221100"));
        Assert.NotNull(ex.InnerException);

        var config = (ScratchBirdConfig)GetPrivateField(connection, "_config")!;
        Assert.Equal(string.Empty, config.DormantId);
        Assert.Equal(string.Empty, config.DormantReattachToken);
        Assert.False((bool)GetPrivateField(connection, "_skipSchemaApplyOnce")!);
    }

    [Fact]
    public void ReattachDormant_RejectsMissingOrInvalidIdentifiers()
    {
        using var connection = CreateOpenConnection();

        var invalidUuid = Assert.Throws<ScratchBirdSyntaxException>(() =>
            connection.ReattachDormant("not-a-uuid", "ffeeddcc-bbaa-9988-7766-554433221100"));
        Assert.Equal("42601", invalidUuid.SqlState);

        var missingToken = Assert.Throws<ScratchBirdSyntaxException>(() =>
            connection.ReattachDormant("00112233-4455-6677-8899-aabbccddeeff", null));
        Assert.Equal("42601", missingToken.SqlState);
    }

    [Fact]
    public void ResumeSuspendedPortal_RequiresExplicitSuspendedState()
    {
        var client = new ProtocolClient();
        SetPrivateField(client, "_connected", true);
        SetPrivateField(client, "_stream", new ProtocolCaptureStream());

        var ex = Assert.Throws<ScratchBirdTransactionException>(() => client.ResumeSuspendedPortal(2));
        Assert.Equal("55000", ex.SqlState);
    }

    [Fact]
    public void ResumeSuspendedPortal_WritesExecuteOnlyAfterExplicitSuspendedState()
    {
        var client = new ProtocolClient();
        var stream = new ProtocolCaptureStream();
        SetPrivateField(client, "_connected", true);
        SetPrivateField(client, "_stream", stream);

        client.AllowPortalResume();
        client.ResumeSuspendedPortal(4);

        var messages = ParseWrittenMessages(stream.WrittenBytes);
        Assert.Single(messages);
        Assert.Equal(MessageType.EXECUTE, (MessageType)messages[0].Header.Type);
        Assert.Equal(4u, BinaryPrimitives.ReadUInt32LittleEndian(messages[0].Payload.AsSpan(4, 4)));
    }

    private static ScratchBirdConnection CreateOpenConnection()
    {
        var connection = new ScratchBirdConnection("Host=localhost;Port=13092;Database=main;Username=sb_admin;Password=SbAdmin_Compat1!;Pooling=false");
        SetPrivateField(connection, "_state", ConnectionState.Open);
        return connection;
    }

    private static ScratchBirdConnection CreateOpenConnectionWithHealthyClient(ProtocolCaptureStream stream)
    {
        var connection = CreateOpenConnection();
        var client = new ProtocolClient();
        SetPrivateField(client, "_connected", true);
        SetPrivateField(client, "_stream", stream);
        SetPrivateField(connection, "_client", client);
        return connection;
    }

    private static ProtocolMessage BuildReadyMessage(uint sequence, ulong txnId)
    {
        var payload = new byte[20];
        payload[0] = 0;
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(4, 8), txnId);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(12, 8), txnId);
        return new ProtocolMessage(
            new MessageHeader((byte)MessageType.READY, 0, (uint)payload.Length, sequence, new byte[16], txnId),
            payload);
    }

    private static ProtocolMessage BuildParameterStatusMessage(uint sequence, string name, string value)
    {
        var nameBytes = Encoding.UTF8.GetBytes(name);
        var valueBytes = Encoding.UTF8.GetBytes(value);
        var payload = new byte[8 + nameBytes.Length + valueBytes.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(0, 4), (uint)nameBytes.Length);
        Buffer.BlockCopy(nameBytes, 0, payload, 4, nameBytes.Length);
        var valueOffset = 4 + nameBytes.Length;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(valueOffset, 4), (uint)valueBytes.Length);
        Buffer.BlockCopy(valueBytes, 0, payload, valueOffset + 4, valueBytes.Length);
        return new ProtocolMessage(
            new MessageHeader((byte)MessageType.PARAMETER_STATUS, 0, (uint)payload.Length, sequence, new byte[16], 0),
            payload);
    }

    private static List<ProtocolMessage> ParseWrittenMessages(byte[] buffer)
    {
        var messages = new List<ProtocolMessage>();
        var offset = 0;
        while (offset < buffer.Length)
        {
            var header = ProtocolMessage.ParseHeader(buffer.AsSpan(offset, ProtocolConstants.HeaderSize));
            offset += ProtocolConstants.HeaderSize;
            var payload = new byte[header.Length];
            Buffer.BlockCopy(buffer, offset, payload, 0, (int)header.Length);
            offset += (int)header.Length;
            messages.Add(new ProtocolMessage(header, payload));
        }
        return messages;
    }

    private static string ParseQuerySql(byte[] payload)
    {
        if (payload.Length <= 12)
        {
            return string.Empty;
        }

        var terminator = Array.IndexOf(payload, (byte)0, 12);
        var end = terminator >= 0 ? terminator : payload.Length;
        return Encoding.UTF8.GetString(payload, 12, end - 12);
    }

    private static void SetPrivateField(object target, string fieldName, object? value)
    {
        var field = target.GetType().GetField(fieldName, BindingFlags.Instance | BindingFlags.NonPublic);
        Assert.NotNull(field);
        field!.SetValue(target, value);
    }

    private static object? GetPrivateField(object target, string fieldName)
    {
        var field = target.GetType().GetField(fieldName, BindingFlags.Instance | BindingFlags.NonPublic);
        Assert.NotNull(field);
        return field!.GetValue(target);
    }

    private static object? InvokePrivateMethod(object target, string methodName, params object?[]? args)
    {
        var method = target.GetType().GetMethod(methodName, BindingFlags.Instance | BindingFlags.NonPublic);
        Assert.NotNull(method);
        return method!.Invoke(target, args);
    }

    private sealed class ProtocolCaptureStream : Stream
    {
        private readonly MemoryStream _reads;
        private readonly MemoryStream _writes = new();

        public ProtocolCaptureStream(params ProtocolMessage[] responses)
        {
            _reads = new MemoryStream();
            foreach (var response in responses)
            {
                var bytes = response.ToBytes();
                _reads.Write(bytes, 0, bytes.Length);
            }
            _reads.Position = 0;
        }

        public byte[] WrittenBytes => _writes.ToArray();

        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => true;
        public override long Length => _reads.Length;
        public override long Position
        {
            get => _reads.Position;
            set => throw new NotSupportedException();
        }

        public override void Flush()
        {
        }

        public override int Read(byte[] buffer, int offset, int count)
        {
            return _reads.Read(buffer, offset, count);
        }

        public override long Seek(long offset, SeekOrigin origin)
        {
            throw new NotSupportedException();
        }

        public override void SetLength(long value)
        {
            throw new NotSupportedException();
        }

        public override void Write(byte[] buffer, int offset, int count)
        {
            _writes.Write(buffer, offset, count);
        }
    }
}
