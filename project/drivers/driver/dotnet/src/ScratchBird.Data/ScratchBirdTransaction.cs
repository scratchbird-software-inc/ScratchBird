// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Data;
using System.Data.Common;
using System;

namespace ScratchBird.Data;

public sealed class ScratchBirdTransaction : DbTransaction
{
    private readonly ScratchBirdConnection _connection;
    private readonly IsolationLevel _isolationLevel;
    private bool _completed;
    private readonly List<string> _savepoints = new();
    private bool _disposed;

    internal ScratchBirdTransaction(ScratchBirdConnection connection, IsolationLevel isolationLevel)
    {
        _connection = connection;
        _isolationLevel = isolationLevel;
    }

    public override IsolationLevel IsolationLevel => _isolationLevel;

    protected override DbConnection DbConnection => _connection;
    internal bool IsCompleted => _completed;
    internal bool IsDisposed => _disposed;
    internal bool BelongsTo(ScratchBirdConnection connection) => ReferenceEquals(_connection, connection);

    public override void Commit()
    {
        if (_completed)
        {
            return;
        }
        EnsureTransactionActive();
        _connection.GetConnectedClient().Commit();
        _completed = true;
        _savepoints.Clear();
        _connection.CompleteTransaction(this);
    }

    public override void Rollback()
    {
        if (_completed)
        {
            return;
        }
        EnsureTransactionActive();
        _connection.GetConnectedClient().Rollback();
        _completed = true;
        _savepoints.Clear();
        _connection.CompleteTransaction(this);
    }

    public override void Save(string savepointName)
    {
        EnsureTransactionActive();
        if (string.IsNullOrWhiteSpace(savepointName))
        {
            throw new ArgumentException("Savepoint name is required", nameof(savepointName));
        }
        _connection.GetConnectedClient().Savepoint(savepointName);
        _savepoints.Add(savepointName);
    }

    public override void Rollback(string savepointName)
    {
        EnsureTransactionActive();
        if (string.IsNullOrWhiteSpace(savepointName))
        {
            throw new ArgumentException("Savepoint name is required", nameof(savepointName));
        }
        var index = FindSavepoint(savepointName);
        if (index < 0)
        {
            throw new InvalidOperationException($"Savepoint '{savepointName}' not found");
        }
        _connection.GetConnectedClient().RollbackToSavepoint(savepointName);
        if (index < _savepoints.Count - 1)
        {
            _savepoints.RemoveRange(index + 1, _savepoints.Count - index - 1);
        }
    }

    public override void Release(string savepointName)
    {
        EnsureTransactionActive();
        if (string.IsNullOrWhiteSpace(savepointName))
        {
            throw new ArgumentException("Savepoint name is required", nameof(savepointName));
        }
        var index = FindSavepoint(savepointName);
        if (index < 0)
        {
            throw new InvalidOperationException($"Savepoint '{savepointName}' not found");
        }
        _connection.GetConnectedClient().ReleaseSavepoint(savepointName);
        _savepoints.RemoveRange(index, _savepoints.Count - index);
    }

    protected override void Dispose(bool disposing)
    {
        if (_disposed)
        {
            return;
        }

        if (disposing && !_completed)
        {
            try
            {
                Rollback();
            }
            catch (Exception ex) when (ex is ScratchBirdException || ex is InvalidOperationException)
            {
                // Allow dispose to complete and avoid re-throwing after user-initiated disposal.
            }
        }

        if (_completed)
        {
            _connection.CompleteTransaction(this);
        }

        _disposed = true;
        base.Dispose(disposing);
    }

    private void EnsureTransactionActive()
    {
        if (_disposed)
        {
            throw new ObjectDisposedException(nameof(ScratchBirdTransaction));
        }
        if (_completed)
        {
            throw new InvalidOperationException("Transaction is already completed");
        }
        if (_connection.State != ConnectionState.Open)
        {
            throw new InvalidOperationException("Connection is not open");
        }
        if (!_connection.IsActiveTransaction(this))
        {
            throw new InvalidOperationException("Transaction is not active on this connection");
        }
    }

    private int FindSavepoint(string savepointName)
    {
        for (var i = _savepoints.Count - 1; i >= 0; i--)
        {
            if (string.Equals(_savepoints[i], savepointName, StringComparison.OrdinalIgnoreCase))
            {
                return i;
            }
        }

        return -1;
    }
}
