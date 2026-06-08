// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Data;
using System.Data.Common;
using System.Diagnostics;
using System.Linq;
using System.Text;

namespace ScratchBird.Data;

public sealed class ScratchBirdCommand : DbCommand
{
    private string _commandText = string.Empty;
    private int _commandTimeout = 30;
    private CommandType _commandType = CommandType.Text;
    private bool _designTimeVisible;
    private UpdateRowSource _updatedRowSource = UpdateRowSource.Both;
    private ScratchBirdConnection? _connection;
    private ScratchBirdTransaction? _transaction;
    private int _fetchSize;
    private readonly ScratchBirdParameterCollection _parameters = new();
    private NormalizedQuery? _preparedQuery;

    public ScratchBirdCommand() { }

    public ScratchBirdCommand(string commandText)
    {
        _commandText = commandText;
    }

    public ScratchBirdCommand(string commandText, ScratchBirdConnection connection)
    {
        _commandText = commandText;
        _connection = connection;
    }

    public ScratchBirdCommand(string commandText, ScratchBirdConnection connection, ScratchBirdTransaction transaction)
    {
        _commandText = commandText;
        _connection = connection;
        _transaction = transaction;
    }

    public override string CommandText
    {
        get => _commandText;
        set
        {
            _commandText = value ?? string.Empty;
            _preparedQuery = null;
        }
    }

    public override int CommandTimeout
    {
        get => _commandTimeout;
        set
        {
            if (value < 0)
            {
                throw new ArgumentOutOfRangeException(nameof(value), "CommandTimeout must be non-negative");
            }

            _commandTimeout = value;
        }
    }

    public override CommandType CommandType
    {
        get => _commandType;
        set
        {
            if (value != CommandType.Text && value != CommandType.StoredProcedure)
            {
                throw new NotSupportedException("Only CommandType.Text and CommandType.StoredProcedure are supported");
            }
            _commandType = value;
        }
    }

    public override bool DesignTimeVisible
    {
        get => _designTimeVisible;
        set => _designTimeVisible = value;
    }

    public override UpdateRowSource UpdatedRowSource
    {
        get => _updatedRowSource;
        set => _updatedRowSource = value;
    }

    public int FetchSize
    {
        get => _fetchSize;
        set => _fetchSize = Math.Max(0, value);
    }

    public new ScratchBirdConnection? Connection
    {
        get => _connection;
        set => _connection = value;
    }

    protected override DbConnection? DbConnection
    {
        get => _connection;
        set => _connection = value as ScratchBirdConnection;
    }

    public new ScratchBirdTransaction? Transaction
    {
        get => _transaction;
        set => _transaction = value;
    }

    protected override DbTransaction? DbTransaction
    {
        get => _transaction;
        set => _transaction = value as ScratchBirdTransaction;
    }

    public new ScratchBirdParameterCollection Parameters => _parameters;

    protected override DbParameterCollection DbParameterCollection => _parameters;

    public override void Prepare()
    {
        if (_commandType != CommandType.Text && _commandType != CommandType.StoredProcedure)
        {
            throw new NotSupportedException("Prepare only supports CommandType.Text and CommandType.StoredProcedure");
        }
        ValidateCommandExecutionState();

        _preparedQuery = BuildNormalizedQuery();
    }

    public override int ExecuteNonQuery()
    {
        return TrackCommandOperation("Command.ExecuteNonQuery", ExecuteNonQueryCore);
    }

    private int ExecuteNonQueryCore()
    {
        using var reader = ExecuteDbDataReaderCore(CommandBehavior.SingleResult);
        while (reader.Read())
        {
        }
        return reader.RecordsAffected;
    }

    public override async Task<int> ExecuteNonQueryAsync(CancellationToken cancellationToken)
    {
        if (cancellationToken.IsCancellationRequested)
        {
            throw new OperationCanceledException(cancellationToken);
        }

        using var cancellation = cancellationToken.Register(Cancel);
        try
        {
            return await Task.Run(ExecuteNonQuery, cancellationToken);
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception)
        {
            if (cancellationToken.IsCancellationRequested)
            {
                throw new OperationCanceledException(cancellationToken);
            }
            throw;
        }
    }

    public override object? ExecuteScalar()
    {
        return TrackCommandOperation("Command.ExecuteScalar", () =>
        {
            using var reader = ExecuteDbDataReaderCore(CommandBehavior.SingleResult);
            object? first = null;
            if (reader.Read())
            {
                first = reader.GetValue(0);
            }

            while (reader.Read())
            {
            }

            return first;
        });
    }

    public override async Task<object?> ExecuteScalarAsync(CancellationToken cancellationToken)
    {
        if (cancellationToken.IsCancellationRequested)
        {
            throw new OperationCanceledException(cancellationToken);
        }

        using var cancellation = cancellationToken.Register(Cancel);
        try
        {
            return await Task.Run(ExecuteScalar, cancellationToken);
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception)
        {
            if (cancellationToken.IsCancellationRequested)
            {
                throw new OperationCanceledException(cancellationToken);
            }
            throw;
        }
    }

    public new ScratchBirdDataReader ExecuteReader()
    {
        return (ScratchBirdDataReader)ExecuteDbDataReader(CommandBehavior.Default);
    }

    public new ScratchBirdDataReader ExecuteReader(CommandBehavior behavior)
    {
        return (ScratchBirdDataReader)ExecuteDbDataReader(behavior);
    }

    protected override DbDataReader ExecuteDbDataReader(CommandBehavior behavior)
    {
        return TrackCommandOperation("Command.ExecuteReader", () => ExecuteDbDataReaderCore(behavior));
    }

    private ScratchBirdDataReader ExecuteDbDataReaderCore(CommandBehavior behavior)
    {
        ValidateCommandExecutionState();

        var connection = _connection!;
        var client = connection.GetConnectedClient();
        var normalized = BuildNormalizedQuery();
        if (normalized.Parameters.Count == 0)
        {
            var statements = SplitSqlStatements(normalized.Sql);
            if (statements.Count > 1)
            {
                var bufferedOutputMapper = CreateCallableOutputMapper();
                var resultSets = connection.QueryMulti(normalized.Sql, normalized.Parameters, _commandTimeout, _fetchSize);
                return new ScratchBirdDataReader(resultSets, behavior, connection, bufferedOutputMapper);
            }
        }
        if (_preparedQuery == null
            || !string.Equals(_preparedQuery.Sql, normalized.Sql, StringComparison.Ordinal)
            || _preparedQuery.Parameters.Count != normalized.Parameters.Count)
        {
            _preparedQuery = normalized;
        }
        var timeoutMs = _commandTimeout > 0 ? _commandTimeout * 1000 : 0;
        var maxRows = _fetchSize > 0 ? _fetchSize : connection.Config.DefaultFetchSize;
        var outputMapper = CreateCallableOutputMapper();
        var stream = client.ExecuteQuery(normalized.Sql, normalized.Parameters, timeoutMs, maxRows);
        return new ScratchBirdDataReader(stream, behavior, connection, outputMapper);
    }

    private static IReadOnlyList<string> SplitSqlStatements(string sql)
    {
        if (string.IsNullOrWhiteSpace(sql))
        {
            return Array.Empty<string>();
        }

        var statements = new List<string>();
        var builder = new StringBuilder();
        var inSingle = false;
        var inDouble = false;
        for (var i = 0; i < sql.Length; i++)
        {
            var ch = sql[i];
            if (ch == '\'' && !inDouble)
            {
                builder.Append(ch);
                if (inSingle && i + 1 < sql.Length && sql[i + 1] == '\'')
                {
                    builder.Append('\'');
                    i++;
                    continue;
                }
                inSingle = !inSingle;
                continue;
            }
            if (ch == '"' && !inSingle)
            {
                builder.Append(ch);
                if (inDouble && i + 1 < sql.Length && sql[i + 1] == '"')
                {
                    builder.Append('"');
                    i++;
                    continue;
                }
                inDouble = !inDouble;
                continue;
            }
            if (!inSingle && !inDouble && ch == ';')
            {
                var statement = builder.ToString().Trim();
                if (statement.Length > 0)
                {
                    statements.Add(statement);
                }
                builder.Clear();
                continue;
            }
            builder.Append(ch);
        }

        var trailing = builder.ToString().Trim();
        if (trailing.Length > 0)
        {
            statements.Add(trailing);
        }

        return statements;
    }

    private NormalizedQuery NormalizeParameters()
    {
        return SqlHelpers.Normalize(_commandText, _parameters.Cast<ScratchBirdParameter>().ToList());
    }

    private NormalizedQuery BuildNormalizedQuery()
    {
        if (_commandType == CommandType.StoredProcedure)
        {
            return BuildStoredProcedureCall();
        }

        return NormalizeParameters();
    }

    private NormalizedQuery BuildStoredProcedureCall()
    {
        var parameters = _parameters.Cast<ScratchBirdParameter>().ToList();
        var bindParameters = parameters
            .Where(parameter => parameter.Direction != ParameterDirection.ReturnValue)
            .ToList();
        var routine = FormatRoutineName(_commandText);
        var placeholders = bindParameters.Count == 0
            ? string.Empty
            : string.Join(", ", Enumerable.Range(1, bindParameters.Count).Select(index => $"${index}"));
        var sql = placeholders.Length == 0
            ? $"CALL {routine}()"
            : $"CALL {routine}({placeholders})";
        return new NormalizedQuery(sql, bindParameters);
    }

    private static string FormatRoutineName(string routineName)
    {
        var trimmed = routineName.Trim();
        if (trimmed.Length == 0)
        {
            throw new InvalidOperationException("CommandText must be set");
        }

        var segments = trimmed
            .Split('.', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Select(QuoteIdentifier)
            .ToArray();

        if (segments.Length == 0)
        {
            throw new InvalidOperationException("CommandText must be set");
        }

        return string.Join(".", segments);
    }

    private static string QuoteIdentifier(string value)
    {
        if (value.Length >= 2 && value.StartsWith('"') && value.EndsWith('"'))
        {
            return value;
        }

        return $"\"{value.Replace("\"", "\"\"")}\"";
    }

    protected override async Task<DbDataReader> ExecuteDbDataReaderAsync(CommandBehavior behavior, CancellationToken cancellationToken)
    {
        if (cancellationToken.IsCancellationRequested)
        {
            throw new OperationCanceledException(cancellationToken);
        }

        using var cancellation = cancellationToken.Register(Cancel);
        try
        {
            return await Task.Run(() => ExecuteDbDataReader(behavior), cancellationToken);
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception)
        {
            if (cancellationToken.IsCancellationRequested)
            {
                throw new OperationCanceledException(cancellationToken);
            }
            throw;
        }
    }

    public override void Cancel()
    {
        if (_connection == null || _connection.State != ConnectionState.Open)
        {
            return;
        }

        try
        {
            var client = _connection.GetConnectedClient();
            client.Cancel();

            // A canceled stream can leave unread protocol frames; force reconnect on next use.
            client.Close();
        }
        catch
        {
            // best effort; caller requested cancellation and does not need transport details here
        }
    }

    protected override DbParameter CreateDbParameter()
    {
        return new ScratchBirdParameter();
    }

    public new ScratchBirdParameter CreateParameter()
    {
        return new ScratchBirdParameter();
    }

    private T TrackCommandOperation<T>(string operation, Func<T> action)
    {
        var stopwatch = Stopwatch.StartNew();
        var success = false;
        try
        {
            var result = action();
            success = true;
            return result;
        }
        finally
        {
            _connection?.RecordTelemetry(operation, stopwatch.Elapsed, success, _commandText);
        }
    }

    private void ValidateCommandExecutionState()
    {
        if (_connection == null || _connection.State != ConnectionState.Open)
        {
            throw new InvalidOperationException("Connection must be open");
        }

        if (string.IsNullOrWhiteSpace(_commandText))
        {
            throw new InvalidOperationException("CommandText must be set");
        }

        ValidateParameterDirections();

        if (_transaction == null)
        {
            if (_connection.HasActiveTransaction)
            {
                throw new InvalidOperationException("Command requires an explicit Transaction when the connection has an active transaction");
            }

            return;
        }

        if (!_transaction.BelongsTo(_connection))
        {
            throw new InvalidOperationException("Transaction is not associated with this command's connection");
        }

        if (_transaction.IsDisposed)
        {
            throw new InvalidOperationException("Transaction is disposed");
        }

        if (_transaction.IsCompleted)
        {
            throw new InvalidOperationException("Transaction is already completed");
        }

        if (!_connection.IsActiveTransaction(_transaction))
        {
            throw new InvalidOperationException("Transaction is not active on this connection");
        }
    }

    private void ValidateParameterDirections()
    {
        var parameters = _parameters.Cast<ScratchBirdParameter>().ToList();
        if (parameters.Count == 0)
        {
            return;
        }

        if (_commandType != CommandType.StoredProcedure)
        {
            if (parameters.Any(parameter => parameter.Direction != ParameterDirection.Input))
            {
                throw new NotSupportedException("Output, InputOutput, and ReturnValue parameters require CommandType.StoredProcedure");
            }
            return;
        }

        if (parameters.Count(parameter => parameter.Direction == ParameterDirection.ReturnValue) > 1)
        {
            throw new InvalidOperationException("StoredProcedure commands support at most one ReturnValue parameter");
        }

        foreach (var parameter in parameters)
        {
            if (parameter.Direction != ParameterDirection.Input
                && parameter.Direction != ParameterDirection.Output
                && parameter.Direction != ParameterDirection.InputOutput
                && parameter.Direction != ParameterDirection.ReturnValue)
            {
                throw new NotSupportedException($"Unsupported parameter direction: {parameter.Direction}");
            }
        }
    }

    private Action<object?[]?>? CreateCallableOutputMapper()
    {
        if (_commandType != CommandType.StoredProcedure)
        {
            return null;
        }

        var parameters = _parameters.Cast<ScratchBirdParameter>().ToList();
        if (!parameters.Any(IsOutputDirection))
        {
            return null;
        }

        var mapped = false;
        return row =>
        {
            if (mapped)
            {
                return;
            }

            mapped = true;
            ApplyCallableOutputValues(parameters, row);
        };
    }

    private static bool IsOutputDirection(ScratchBirdParameter parameter)
    {
        return parameter.Direction == ParameterDirection.Output
            || parameter.Direction == ParameterDirection.InputOutput
            || parameter.Direction == ParameterDirection.ReturnValue;
    }

    private static void ApplyCallableOutputValues(IReadOnlyList<ScratchBirdParameter> parameters, object?[]? firstRow)
    {
        var returnValue = parameters.FirstOrDefault(parameter => parameter.Direction == ParameterDirection.ReturnValue);
        var outputParameters = parameters
            .Where(parameter =>
                parameter.Direction == ParameterDirection.Output
                || parameter.Direction == ParameterDirection.InputOutput)
            .ToList();

        var column = 0;
        if (returnValue != null)
        {
            returnValue.Value = ResolveCallableOutputValue(firstRow, column, returnValue);
            column++;
        }

        foreach (var outputParameter in outputParameters)
        {
            outputParameter.Value = ResolveCallableOutputValue(firstRow, column, outputParameter);
            column++;
        }
    }

    private static object? ResolveCallableOutputValue(object?[]? firstRow, int column, ScratchBirdParameter parameter)
    {
        if (firstRow == null || column < 0 || column >= firstRow.Length)
        {
            if (parameter.Direction == ParameterDirection.InputOutput)
            {
                return parameter.Value;
            }

            return DBNull.Value;
        }

        var value = firstRow[column];
        return value ?? DBNull.Value;
    }

}
