// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Collections;
using System.Data;
using System.Data.Common;
using System.IO;
using System.Text;

namespace ScratchBird.Data;

public sealed class ScratchBirdDataReader : DbDataReader
{
    private readonly IReadOnlyList<ResultSetSummary>? _bufferedResults;
    private readonly ProtocolClient.QueryStream? _stream;
    private readonly CommandBehavior _behavior;
    private readonly ScratchBirdConnection? _connection;
    private readonly Action<object?[]?>? _firstResultCompletedCallback;
    private int _bufferedRowIndex;
    private object?[]? _currentRow;
    private bool _closed;
    private bool _done;
    private bool _resultComplete;
    private bool _prefetched;
    private int _recordsAffected;
    private int _resultIndex;
    private object?[]? _firstResultFirstRow;
    private bool _firstResultCallbackInvoked;

    internal ScratchBirdDataReader(
        ProtocolClient.QueryStream stream,
        CommandBehavior behavior,
        ScratchBirdConnection? connection,
        Action<object?[]?>? firstResultCompletedCallback = null)
    {
        _stream = stream;
        _behavior = behavior;
        _connection = connection;
        _firstResultCompletedCallback = firstResultCompletedCallback;
        _recordsAffected = -1;
    }

    internal ScratchBirdDataReader(
        IReadOnlyList<ResultSetSummary> bufferedResults,
        CommandBehavior behavior,
        ScratchBirdConnection? connection,
        Action<object?[]?>? firstResultCompletedCallback = null)
    {
        _bufferedResults = bufferedResults;
        _behavior = behavior;
        _connection = connection;
        _firstResultCompletedCallback = firstResultCompletedCallback;
        _recordsAffected = -1;
        _done = bufferedResults.Count == 0;
        _resultComplete = bufferedResults.Count == 0;
    }

    public override object this[int ordinal] => GetValue(ordinal);
    public override object this[string name] => GetValue(GetOrdinal(name));

    public override int Depth => 0;
    public override bool IsClosed => _closed;
    public override int RecordsAffected => _recordsAffected;
    public override int FieldCount => UsingBufferedResults
        ? CurrentBufferedFields.Count
        : _stream!.Columns.Count;

    public override bool HasRows
    {
        get
        {
            if (_closed || _done || _resultComplete)
            {
                return false;
            }
            if (_currentRow != null)
            {
                return true;
            }
            if (UsingBufferedResults)
            {
                return BufferedHasRows();
            }
            var row = _stream.ReadNextRow();
            if (row != null)
            {
                CaptureFirstResultRow(row);
                _currentRow = row;
                _prefetched = true;
                return true;
            }
            MarkCurrentResultComplete();
            return false;
        }
    }

    public override bool Read()
    {
        if (_closed)
        {
            return false;
        }
        if (_done)
        {
            return false;
        }
        if (_resultComplete)
        {
            return false;
        }
        if (UsingBufferedResults)
        {
            return BufferedRead();
        }
        if (_prefetched)
        {
            _prefetched = false;
            return true;
        }
        _currentRow = null;
        var row = _stream.ReadNextRow();
        if (row == null)
        {
            MarkCurrentResultComplete();
            return false;
        }
        CaptureFirstResultRow(row);
        _currentRow = row;
        return true;
    }

    public override async Task<bool> ReadAsync(CancellationToken cancellationToken)
    {
        if (cancellationToken.IsCancellationRequested)
        {
            throw new OperationCanceledException(cancellationToken);
        }
        using var registration = cancellationToken.Register(() =>
        {
            try
            {
                if (!_done)
                {
                    _stream?.Cancel();
                }
            }
            catch
            {
                // best effort cleanup only; ignore transport-level cancellation errors
            }
        });
        try
        {
            return await Task.Run(Read, cancellationToken);
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

    public override bool NextResult()
    {
        if (_closed || _done)
        {
            return false;
        }

        _currentRow = null;
        _prefetched = false;

        if (UsingBufferedResults)
        {
            return BufferedNextResult();
        }

        var completingFirstResult = _resultIndex == 0 && !_firstResultCallbackInvoked;
        var moved = _stream!.NextResult();
        if (completingFirstResult)
        {
            TryInvokeFirstResultCallback();
        }
        if (!moved)
        {
            _done = true;
            _resultComplete = true;
            _recordsAffected = SaturatingLongToInt(_stream.RowsAffected);
            return false;
        }

        _resultIndex++;
        _resultComplete = false;
        _recordsAffected = -1;
        return true;
    }

    public override async Task<bool> NextResultAsync(CancellationToken cancellationToken)
    {
        if (cancellationToken.IsCancellationRequested)
        {
            throw new OperationCanceledException(cancellationToken);
        }

        using var registration = cancellationToken.Register(() =>
        {
            try
            {
                if (!_done)
                {
                    _stream?.Cancel();
                }
            }
            catch
            {
                // best effort cleanup only; ignore transport-level cancellation errors
            }
        });

        try
        {
            return await Task.Run(NextResult, cancellationToken);
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

    public override string GetName(int ordinal)
    {
        return UsingBufferedResults
            ? CurrentBufferedFields[ordinal].Name
            : _stream!.Columns[ordinal].Name;
    }

    public override int GetOrdinal(string name)
    {
        var fields = UsingBufferedResults
            ? CurrentBufferedFields.Select(field => field.Name).ToArray()
            : _stream!.Columns.Select(column => column.Name).ToArray();
        for (var i = 0; i < fields.Length; i++)
        {
            if (string.Equals(fields[i], name, StringComparison.OrdinalIgnoreCase))
            {
                return i;
            }
        }
        return -1;
    }

    public override string GetDataTypeName(int ordinal)
    {
        return UsingBufferedResults
            ? TypeDecoder.OidToString(CurrentBufferedFields[ordinal].TypeOid)
            : TypeDecoder.OidToString(_stream!.Columns[ordinal].TypeOid);
    }

    public override Type GetFieldType(int ordinal)
    {
        return UsingBufferedResults
            ? TypeDecoder.GetClrType(CurrentBufferedFields[ordinal].TypeOid)
            : TypeDecoder.GetClrType(_stream!.Columns[ordinal].TypeOid);
    }

    public override object GetValue(int ordinal)
    {
        EnsureRow();
        return _currentRow![ordinal] ?? DBNull.Value;
    }

    public override int GetValues(object[] values)
    {
        EnsureRow();
        var count = Math.Min(values.Length, _currentRow!.Length);
        for (var i = 0; i < count; i++)
        {
            values[i] = _currentRow[i] ?? DBNull.Value;
        }
        return count;
    }

    public override bool IsDBNull(int ordinal)
    {
        EnsureRow();
        return _currentRow![ordinal] == null || _currentRow![ordinal] == DBNull.Value;
    }

    public override bool GetBoolean(int ordinal) => (bool)GetValue(ordinal);
    public override byte GetByte(int ordinal) => (byte)GetValue(ordinal);
    public override long GetBytes(int ordinal, long dataOffset, byte[]? buffer, int bufferOffset, int length)
    {
        if (GetValue(ordinal) is not byte[] data)
        {
            throw new InvalidCastException("Column value is not a byte array");
        }
        if (dataOffset < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(dataOffset), "Data offset cannot be negative");
        }
        if (length < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(length), "Length cannot be negative");
        }
        if (buffer != null)
        {
            if (bufferOffset < 0)
            {
                throw new ArgumentOutOfRangeException(nameof(bufferOffset), "Buffer offset cannot be negative");
            }
            if (bufferOffset > buffer.Length)
            {
                throw new ArgumentOutOfRangeException(nameof(bufferOffset), "Buffer offset is beyond buffer length");
            }
        }
        var available = data.Length - (int)dataOffset;
        if (available <= 0)
        {
            return 0;
        }
        var toCopy = Math.Min(available, length);
        if (buffer != null)
        {
            if (bufferOffset + toCopy > buffer.Length)
            {
                throw new ArgumentException("The buffer is too small for the requested read");
            }
            Buffer.BlockCopy(data, (int)dataOffset, buffer, bufferOffset, toCopy);
        }
        if (buffer == null)
        {
            return available;
        }
        return toCopy;
    }

    public override char GetChar(int ordinal) => (char)GetValue(ordinal);

    public override Stream GetStream(int ordinal)
    {
        var value = GetValue(ordinal);
        if (value == null || value == DBNull.Value)
        {
            throw new InvalidCastException("Column value is NULL");
        }
        return value is byte[] bytes ? new MemoryStream(bytes, writable: false) : new MemoryStream(Encoding.UTF8.GetBytes(Convert.ToString(value)!));
    }

    public override TextReader GetTextReader(int ordinal)
    {
        var value = GetValue(ordinal);
        if (value == null || value == DBNull.Value)
        {
            throw new InvalidCastException("Column value is NULL");
        }
        return value is string text ? new StringReader(text) : new StringReader(Convert.ToString(value)!);
    }

    public Task<Stream> GetStreamAsync(int ordinal, CancellationToken cancellationToken)
    {
        return Task.FromResult(GetStream(ordinal));
    }

    public Task<TextReader> GetTextReaderAsync(int ordinal, CancellationToken cancellationToken)
    {
        return Task.FromResult(GetTextReader(ordinal));
    }

    public override long GetChars(int ordinal, long dataOffset, char[]? buffer, int bufferOffset, int length)
    {
        if (dataOffset < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(dataOffset), "Data offset cannot be negative");
        }
        if (length < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(length), "Length cannot be negative");
        }
        if (buffer != null)
        {
            if (bufferOffset < 0)
            {
                throw new ArgumentOutOfRangeException(nameof(bufferOffset), "Buffer offset cannot be negative");
            }
            if (bufferOffset > buffer.Length)
            {
                throw new ArgumentOutOfRangeException(nameof(bufferOffset), "Buffer offset is beyond buffer length");
            }
        }

        var data = GetString(ordinal).ToCharArray();
        var available = data.Length - (int)dataOffset;
        if (available <= 0)
        {
            return 0;
        }
        var toCopy = Math.Min(available, length);
        if (buffer != null)
        {
            if (bufferOffset + toCopy > buffer.Length)
            {
                throw new ArgumentException("The buffer is too small for the requested read");
            }
            Array.Copy(data, (int)dataOffset, buffer, bufferOffset, toCopy);
        }
        if (buffer == null)
        {
            return available;
        }
        return toCopy;
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            Close();
        }

        base.Dispose(disposing);
    }

    public override Guid GetGuid(int ordinal) => (Guid)GetValue(ordinal);
    public override short GetInt16(int ordinal) => Convert.ToInt16(GetValue(ordinal));
    public override int GetInt32(int ordinal) => Convert.ToInt32(GetValue(ordinal));
    public override long GetInt64(int ordinal) => Convert.ToInt64(GetValue(ordinal));
    public override float GetFloat(int ordinal) => Convert.ToSingle(GetValue(ordinal));
    public override double GetDouble(int ordinal) => Convert.ToDouble(GetValue(ordinal));
    public override string GetString(int ordinal) => Convert.ToString(GetValue(ordinal)) ?? string.Empty;
    public override decimal GetDecimal(int ordinal) => Convert.ToDecimal(GetValue(ordinal));
    public override DateTime GetDateTime(int ordinal) => Convert.ToDateTime(GetValue(ordinal));

    public override IEnumerator GetEnumerator()
    {
        while (Read())
        {
            yield return _currentRow!;
        }
    }

    public override void Close()
    {
        if (_closed)
        {
            return;
        }
        if (!_done)
        {
            if (UsingBufferedResults)
            {
                _done = true;
                _resultComplete = true;
            }
            else if (_behavior.HasFlag(CommandBehavior.SingleRow))
            {
                try
                {
                    while (_stream!.ReadNextRow() != null)
                    {
                    }
                    while (_stream.NextResult())
                    {
                        while (_stream.ReadNextRow() != null)
                        {
                        }
                    }
                    _done = true;
                    _resultComplete = true;
                    _recordsAffected = SaturatingLongToInt(_stream.RowsAffected);
                }
                catch
                {
                    _stream!.Cancel();
                }
            }
            else
            {
                _stream?.Cancel();
            }
        }
        TryInvokeFirstResultCallback();
        _stream?.Dispose();
        _closed = true;
        if (_behavior.HasFlag(CommandBehavior.CloseConnection))
        {
            _connection?.Close();
        }
    }

    private void EnsureRow()
    {
        if (_currentRow == null)
        {
            throw new InvalidOperationException("No current row");
        }
    }

    private void MarkCurrentResultComplete()
    {
        _resultComplete = true;
        if (UsingBufferedResults)
        {
            _recordsAffected = SaturatingLongToInt(CurrentBufferedResult.RowCount);
        }
        else
        {
            _recordsAffected = SaturatingLongToInt(_stream!.RowsAffected);
            _done = _stream.IsDone;
        }
        TryInvokeFirstResultCallback();
    }

    private static int SaturatingLongToInt(long value)
    {
        if (value < int.MinValue)
        {
            return int.MinValue;
        }

        if (value > int.MaxValue)
        {
            return int.MaxValue;
        }

        return (int)value;
    }

    private void CaptureFirstResultRow(object?[] row)
    {
        if (_resultIndex != 0 || _firstResultFirstRow != null)
        {
            return;
        }

        var copy = new object?[row.Length];
        Array.Copy(row, copy, row.Length);
        _firstResultFirstRow = copy;
    }

    private void TryInvokeFirstResultCallback()
    {
        if (_firstResultCallbackInvoked || _resultIndex != 0)
        {
            return;
        }

        _firstResultCallbackInvoked = true;
        _firstResultCompletedCallback?.Invoke(_firstResultFirstRow);
    }

    private bool UsingBufferedResults => _bufferedResults != null;

    private ResultSetSummary CurrentBufferedResult => _bufferedResults![_resultIndex];

    private IReadOnlyList<FieldSummary> CurrentBufferedFields => CurrentBufferedResult.Fields;

    private bool BufferedHasRows()
    {
        if (_done || _resultComplete)
        {
            return false;
        }
        if (_currentRow != null)
        {
            return true;
        }
        if (_bufferedRowIndex >= CurrentBufferedResult.Rows.Count)
        {
            MarkCurrentResultComplete();
            return false;
        }
        var row = CurrentBufferedResult.Rows[_bufferedRowIndex++];
        CaptureFirstResultRow(row);
        _currentRow = row;
        _prefetched = true;
        return true;
    }

    private bool BufferedRead()
    {
        if (_prefetched)
        {
            _prefetched = false;
            return true;
        }
        _currentRow = null;
        if (_bufferedRowIndex >= CurrentBufferedResult.Rows.Count)
        {
            MarkCurrentResultComplete();
            return false;
        }
        var row = CurrentBufferedResult.Rows[_bufferedRowIndex++];
        CaptureFirstResultRow(row);
        _currentRow = row;
        return true;
    }

    private bool BufferedNextResult()
    {
        var completingFirstResult = _resultIndex == 0 && !_firstResultCallbackInvoked;
        if (completingFirstResult)
        {
            TryInvokeFirstResultCallback();
        }
        if (_resultIndex + 1 >= _bufferedResults!.Count)
        {
            _done = true;
            _resultComplete = true;
            _recordsAffected = SaturatingLongToInt(CurrentBufferedResult.RowCount);
            return false;
        }

        _resultIndex++;
        _bufferedRowIndex = 0;
        _resultComplete = false;
        _recordsAffected = -1;
        return true;
    }
}
