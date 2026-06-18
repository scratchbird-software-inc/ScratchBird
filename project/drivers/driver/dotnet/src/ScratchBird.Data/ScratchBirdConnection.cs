// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Data;
using System.Data.Common;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

namespace ScratchBird.Data;

public sealed class ScratchBirdConnection : DbConnection
{
    private const int MaxConnectRetries = 3;
    private const int ReconnectBackoffMs = 120;
    private const int MinConnectTimeoutMs = 1;

    private string _connectionString = string.Empty;
    private ConnectionState _state = ConnectionState.Closed;
    private ScratchBirdConfig _config = new();
    private ProtocolClient? _client;
    private ProtocolClientPool.Lease? _clientLease;
    private ScratchBirdTransaction? _activeTransaction;
    private bool _disposed;
    private TelemetryCollector _telemetry = new();
    private CircuitBreaker _circuitBreaker = new();
    private KeepaliveMonitor _keepalive = new();
    private PipelineMonitor _pipelineMonitor = new();
    private LeakMonitor _leakMonitor = new();
    private readonly object _notificationSync = new();
    private Queue<ScratchBirdNotification>? _notificationQueue;
    private HashSet<Action<ScratchBirdNotification>>? _notificationListeners;
    private ProtocolClient? _notificationBridgeClient;
    private bool _notificationBridgeRequested;
    private bool _skipSchemaApplyOnce;

    public ScratchBirdConnection() { }

    public ScratchBirdConnection(string connectionString)
    {
        ConnectionString = connectionString;
    }

    public override string ConnectionString
    {
        get => _connectionString;
        set
        {
            if (_state != ConnectionState.Closed)
            {
                throw new InvalidOperationException("Cannot set ConnectionString while open");
            }
            _connectionString = value;
            _config = ScratchBirdConfig.FromConnectionString(value);
            _telemetry = new TelemetryCollector(BuildTelemetryOptions(_config));
            _circuitBreaker = new CircuitBreaker(BuildCircuitBreakerOptions(_config));
            _keepalive = new KeepaliveMonitor(BuildKeepaliveOptions(_config));
            _pipelineMonitor = new PipelineMonitor(BuildPipelineOptions(_config));
            _leakMonitor = new LeakMonitor(BuildLeakOptions(_config));
        }
    }

    public override string Database => _config.Database;

    public override string DataSource => _config.Host;

    public override string ServerVersion => "1.0";

    public override ConnectionState State => _state;

    internal ProtocolClient Client => _client ?? throw new InvalidOperationException("Connection not open");
    internal ScratchBirdConfig Config => _config;
    internal ProtocolClient GetConnectedClient() => EnsureConnectedClient();

    public ScratchBirdResolvedAuthContext GetResolvedAuthContext()
    {
        return _client?.GetResolvedAuthContext() ?? new ScratchBirdResolvedAuthContext
        {
            FrontDoorMode = _config.FrontDoorMode
        };
    }

    public static ScratchBirdAuthProbeResult ProbeAuthSurface(string connectionString)
    {
        var config = ScratchBirdConfig.FromConnectionString(connectionString);
        var client = new ProtocolClient();
        return client.ProbeAuthSurface(config);
    }

    public override void Open()
    {
        if (_state != ConnectionState.Closed)
        {
            return;
        }

        TrackOperation("Connection.Open", () =>
        {
            OpenWithRetry();
            _state = ConnectionState.Open;
        });
    }

    private void OpenWithRetry(CancellationToken cancellationToken)
    {
        ScratchBirdException? lastFailure = null;

        for (var attempt = 0; attempt < MaxConnectRetries; attempt++)
        {
            cancellationToken.ThrowIfCancellationRequested();
            try
            {
                BorrowAndConnect();
                return;
            }
            catch (ScratchBirdException ex)
            {
                lastFailure = ex;
                _clientLease?.Dispose();
                _clientLease = null;
                _client = null;

                if (attempt + 1 < MaxConnectRetries)
                {
                    var retryDelayMs = Math.Max(MinConnectTimeoutMs, Math.Min(ReconnectBackoffMs * (1 << attempt), 1000));
                    if (cancellationToken.WaitHandle.WaitOne(retryDelayMs))
                    {
                        cancellationToken.ThrowIfCancellationRequested();
                    }
                    continue;
                }

                throw;
            }
        }

        if (lastFailure != null)
        {
            throw lastFailure;
        }
    }

    private void OpenWithRetry()
    {
        OpenWithRetry(CancellationToken.None);
    }

    private void BorrowAndConnect()
    {
        _clientLease?.Dispose();
        _clientLease = null;
        _activeTransaction = null;

        _client = ProtocolClientPool.BorrowOrCreate(_config, out var lease);
        _clientLease = lease;
        try
        {
            if (!_client.IsHealthy)
            {
                _client.Connect(_config);
            }
            _state = ConnectionState.Open;
            _leakMonitor.Checkout();
            if (_skipSchemaApplyOnce)
            {
                _skipSchemaApplyOnce = false;
            }
            else
            {
                ApplySchema();
            }
            InstallNotificationBridgeIfNeeded(_client);
        }
        catch
        {
            _clientLease?.Dispose();
            _clientLease = null;
            _client = null;
            lock (_notificationSync)
            {
                _notificationBridgeClient = null;
            }
            throw;
        }
    }

    private ProtocolClient EnsureConnectedClient()
    {
        if (_state != ConnectionState.Open)
        {
            throw new InvalidOperationException("Connection is not open");
        }

        if (_client == null)
        {
            OpenWithRetry();
            var restored = _client ?? throw new InvalidOperationException("Connection could not be restored");
            EnsureKeepaliveHealthy(restored);
            return restored;
        }

        if (_client.IsHealthy)
        {
            EnsureKeepaliveHealthy(_client);
            return _client;
        }

        // MGA recovery rule: a stale transport can be reopened, but the driver must drop
        // any local claim that an abandoned transaction is still active on the new session.
        _clientLease?.Dispose();
        _clientLease = null;
        _client = null;
        _activeTransaction = null;
        OpenWithRetry();

        var recovered = _client ?? throw new InvalidOperationException("Connection could not be restored");
        EnsureKeepaliveHealthy(recovered);
        return recovered;
    }

    private void EnsureKeepaliveHealthy(ProtocolClient client)
    {
        bool validated;
        try
        {
            validated = _keepalive.ValidateIfNeeded(() => ValidateClientWithTimeout(client));
        }
        catch (ScratchBirdException)
        {
            throw;
        }
        catch (Exception ex)
        {
            throw new ScratchBirdConnectionException("Keepalive validation failed", "08006", ex.Message, null);
        }

        if (!validated)
        {
            throw new ScratchBirdConnectionException("Keepalive validation timed out", "08006");
        }
    }

    private bool ValidateClientWithTimeout(ProtocolClient client)
    {
        var timeoutMs = _config.KeepaliveValidationTimeoutMs;
        if (timeoutMs <= 0)
        {
            client.Ping();
            return true;
        }

        var pingTask = Task.Run(() => client.Ping());
        if (!pingTask.Wait(timeoutMs))
        {
            return false;
        }

        pingTask.GetAwaiter().GetResult();
        return true;
    }

    private void ReconnectWithDormantParams(string dormantId, string dormantReattachToken)
    {
        var priorDormantId = _config.DormantId;
        var priorDormantToken = _config.DormantReattachToken;
        var priorSkipSchema = _skipSchemaApplyOnce;
        _config.DormantId = dormantId;
        _config.DormantReattachToken = dormantReattachToken;
        _skipSchemaApplyOnce = true;
        _client?.Close();
        _clientLease?.Dispose();
        _clientLease = null;
        _client = null;
        _activeTransaction = null;

        try
        {
            OpenWithRetry();
        }
        finally
        {
            _config.DormantId = priorDormantId;
            _config.DormantReattachToken = priorDormantToken;
            _skipSchemaApplyOnce = priorSkipSchema;
        }
    }

    public override async Task OpenAsync(CancellationToken cancellationToken)
    {
        if (_state != ConnectionState.Closed)
        {
            return;
        }

        var stopwatch = Stopwatch.StartNew();
        var success = false;
        try
        {
            await Task.Run(() => OpenWithRetry(cancellationToken), cancellationToken);
            success = true;
        }
        finally
        {
            RecordTelemetry("Connection.OpenAsync", stopwatch.Elapsed, success);
        }
    }

    public override void Close()
    {
        if (_disposed)
        {
            return;
        }

        var lease = _clientLease;
        _clientLease = null;
        _client = null;
        _activeTransaction = null;
        _leakMonitor.Checkin();
        _state = ConnectionState.Closed;
        lock (_notificationSync)
        {
            _notificationBridgeClient = null;
        }
        lease?.Dispose();
    }

    protected override void Dispose(bool disposing)
    {
        if (_disposed)
        {
            base.Dispose(disposing);
            return;
        }

        if (disposing)
        {
            Close();
        }
        _disposed = true;
        base.Dispose(disposing);
    }

    public override void ChangeDatabase(string databaseName)
    {
        if (string.IsNullOrWhiteSpace(databaseName))
        {
            throw new ArgumentException("databaseName is required");
        }
        _config.Database = databaseName;
    }

    protected override DbTransaction BeginDbTransaction(IsolationLevel isolationLevel)
    {
        return TrackOperation("Connection.BeginTransaction", () =>
        {
            if (_state != ConnectionState.Open)
            {
                throw new InvalidOperationException("Connection is not open");
            }
            if (HasActiveTransaction)
            {
                throw new InvalidOperationException("Connection already has an active transaction");
            }

            GetConnectedClient().Begin(isolationLevel);
            var transaction = new ScratchBirdTransaction(this, isolationLevel);
            _activeTransaction = transaction;
            return (DbTransaction)transaction;
        });
    }

    public ScratchBirdTransaction BeginTransaction(ScratchBirdTransactionOptions options)
    {
        return TrackOperation("Connection.BeginTransactionWithOptions", () =>
        {
            ArgumentNullException.ThrowIfNull(options);
            if (_state != ConnectionState.Open)
            {
                throw new InvalidOperationException("Connection is not open");
            }
            if (HasActiveTransaction)
            {
                throw new InvalidOperationException("Connection already has an active transaction");
            }

            GetConnectedClient().Begin(options);
            var transaction = new ScratchBirdTransaction(this, options.IsolationLevel);
            _activeTransaction = transaction;
            return transaction;
        });
    }

    internal bool HasActiveTransaction
    {
        get
        {
            if (_activeTransaction != null && _activeTransaction.IsCompleted)
            {
                _activeTransaction = null;
            }

            return _activeTransaction != null;
        }
    }

    internal bool IsActiveTransaction(ScratchBirdTransaction transaction)
    {
        ArgumentNullException.ThrowIfNull(transaction);
        return ReferenceEquals(_activeTransaction, transaction) && !_activeTransaction.IsCompleted;
    }

    internal void CompleteTransaction(ScratchBirdTransaction transaction)
    {
        if (_activeTransaction != null && ReferenceEquals(_activeTransaction, transaction))
        {
            _activeTransaction = null;
        }
    }

    protected override DbCommand CreateDbCommand()
    {
        return new ScratchBirdCommand { Connection = this };
    }

    public string NativeSql(string sql, IReadOnlyList<ScratchBirdParameter>? parameters = null)
    {
        var normalized = SqlHelpers.Normalize(sql, NormalizeParameterList(parameters));
        return normalized.Sql;
    }

    public string NativeCallableSql(string sql, IReadOnlyList<ScratchBirdParameter>? parameters = null)
    {
        var normalized = SqlHelpers.NormalizeCallable(sql, NormalizeParameterList(parameters));
        return normalized.Sql;
    }

    public bool SupportsPreparedTransactions() => true;

    public bool SupportsDormantReattach() => true;

    public void PrepareTransaction(string globalTransactionId)
    {
        TrackOperation("Connection.PrepareTransaction", () =>
            ExecuteControlCommand(BuildPreparedTransactionSql("PREPARE TRANSACTION", globalTransactionId)));
    }

    public void CommitPrepared(string globalTransactionId)
    {
        TrackOperation("Connection.CommitPrepared", () =>
            ExecuteControlCommand(BuildPreparedTransactionSql("COMMIT PREPARED", globalTransactionId)));
    }

    public void RollbackPrepared(string globalTransactionId)
    {
        TrackOperation("Connection.RollbackPrepared", () =>
            ExecuteControlCommand(BuildPreparedTransactionSql("ROLLBACK PREPARED", globalTransactionId)));
    }

    public (string DormantId, string ReattachToken) DetachToDormant()
    {
        return TrackOperation("Connection.DetachToDormant", () =>
        {
            var client = EnsureConnectedClient();
            client.AttachDetach();
            if (!client.TryGetParameter("dormant_id", out var dormantId) ||
                !client.TryGetParameter("dormant_reattach_token", out var reattachToken) ||
                string.IsNullOrWhiteSpace(dormantId) ||
                string.IsNullOrWhiteSpace(reattachToken))
            {
                throw new ScratchBirdConnectionException(
                    "expected dormant detach identifiers from the server",
                    "08006");
            }

            return (
                NormalizeUuidText(dormantId, "dormant_id"),
                NormalizeUuidText(reattachToken, "dormant_reattach_token"));
        });
    }

    public void ReattachDormant(string dormantId, string? authToken = null)
    {
        TrackOperation("Connection.ReattachDormant", () =>
        {
            if (string.IsNullOrWhiteSpace(authToken))
            {
                throw new ScratchBirdSyntaxException(
                    "dormant reattach requires the engine-issued auth token",
                    "42601");
            }

            ReconnectWithDormantParams(
                NormalizeUuidText(dormantId, "dormant_id"),
                NormalizeUuidText(authToken, "dormant_reattach_token"));
        });
    }

    public ConnectionDiagnosticsSummary GetDiagnostics()
    {
        var client = _client;
        return new ConnectionDiagnosticsSummary(
            DateTimeOffset.UtcNow,
            _state,
            client?.IsHealthy ?? false,
            _config.FrontDoorMode,
            _config.Protocol,
            _config.Host,
            _config.Port,
            _config.Database,
            _config.Pooling,
            GetPoolDiagnostics(),
            CreateQueryPlanSummary(client?.LastPlan),
            CreateSblrSummary(client?.LastSblr),
            MapCircuitBreakerSummary(_circuitBreaker.Snapshot()),
            MapKeepaliveSummary(_keepalive.Snapshot()),
            MapPipelineSummary(_pipelineMonitor.Snapshot()),
            MapLeakSummary(_leakMonitor.Snapshot()));
    }

    public ConnectionTelemetrySummary GetTelemetrySummary()
    {
        return _telemetry.Snapshot();
    }

    public void ResetTelemetry()
    {
        _telemetry.Reset();
    }

    public IReadOnlyList<SlowOperationSummary> GetSlowOperations()
    {
        return _telemetry.GetSlowOperations();
    }

    public string ExportTelemetryPrometheus()
    {
        return _telemetry.ExportPrometheusMetrics();
    }

    public CircuitBreakerSummary GetCircuitBreakerSummary()
    {
        return MapCircuitBreakerSummary(_circuitBreaker.Snapshot());
    }

    public KeepaliveSummary GetKeepaliveSummary()
    {
        return MapKeepaliveSummary(_keepalive.Snapshot());
    }

    public PipelineSummary GetPipelineSummary()
    {
        return MapPipelineSummary(_pipelineMonitor.Snapshot());
    }

    public ScratchBirdQueryPipeline CreateQueryPipeline(ScratchBirdQueryPipelineConfig? config = null)
    {
        return new ScratchBirdQueryPipeline(this, config);
    }

    public async Task<IReadOnlyList<IReadOnlyList<ResultSetSummary>>> ExecutePipelineBatchAsync(
        IReadOnlyList<ScratchBirdPipelineBatchItem> items,
        ScratchBirdQueryPipelineConfig? config = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(items);

        await using var pipeline = CreateQueryPipeline(config);
        return await pipeline.ExecuteBatchAsync(items, cancellationToken).ConfigureAwait(false);
    }

    public LeakSummary GetLeakSummary()
    {
        return MapLeakSummary(_leakMonitor.Snapshot());
    }

    public PoolDiagnosticsSummary? GetPoolDiagnostics()
    {
        return MapPoolDiagnostics(ProtocolClientPool.GetStats(_config));
    }

    public static PoolDiagnosticsSummary? GetPoolDiagnostics(string connectionString)
    {
        var config = ScratchBirdConfig.FromConnectionString(connectionString);
        return MapPoolDiagnostics(ProtocolClientPool.GetStats(config));
    }

    internal void RecordTelemetry(string operation, TimeSpan duration, bool success, string? statement = null)
    {
        _telemetry.Record(operation, duration, success, statement);
    }

    private T TrackOperation<T>(string operation, Func<T> action)
    {
        if (!_circuitBreaker.AllowRequest())
        {
            RecordTelemetry(operation, TimeSpan.Zero, success: false);
            throw new ScratchBirdConnectionException("Circuit breaker is OPEN", "08006");
        }

        if (!_pipelineMonitor.TryAcquire())
        {
            RecordTelemetry(operation, TimeSpan.Zero, success: false);
            throw new ScratchBirdLimitException(
                "Pipeline capacity exceeded",
                "54000",
                $"pipeline_max_in_flight={_config.PipelineMaxInFlight}");
        }

        var stopwatch = Stopwatch.StartNew();
        var success = false;
        try
        {
            var result = action();
            success = true;
            return result;
        }
        catch
        {
            _circuitBreaker.RecordFailure();
            throw;
        }
        finally
        {
            if (success)
            {
                _circuitBreaker.RecordSuccess();
            }

            _pipelineMonitor.Release(success);
            _keepalive.MarkActivity();
            RecordTelemetry(operation, stopwatch.Elapsed, success);
        }
    }

    private void TrackOperation(string operation, Action action)
    {
        _ = TrackOperation(operation, () =>
        {
            action();
            return 0;
        });
    }

    public void Listen(string channel, string filterExpr = "")
    {
        Subscribe(ScratchBirdSubscriptionType.Channel, channel, filterExpr);
    }

    public void Unlisten(string channel)
    {
        Unsubscribe(channel);
    }

    public void UnlistenAll()
    {
        TrackOperation("Connection.UnlistenAll", () => ExecuteControlCommand("UNLISTEN *"));
    }

    public void Subscribe(ScratchBirdSubscriptionType subscriptionType, string channel, string filterExpr = "")
    {
        var normalizedChannel = NormalizeNotificationChannel(channel);
        var normalizedFilter = filterExpr ?? string.Empty;
        TrackOperation("Connection.Subscribe", () =>
        {
            var client = EnsureNotificationBridge();
            client.Subscribe((byte)subscriptionType, normalizedChannel, normalizedFilter);
        });
    }

    public void Unsubscribe(string channel)
    {
        var normalizedChannel = NormalizeNotificationChannel(channel);
        TrackOperation("Connection.Unsubscribe", () =>
        {
            var client = EnsureConnectedClient();
            client.Unsubscribe(normalizedChannel);
        });
    }

    public void NotifyChannel(string channel)
    {
        NotifyChannel(channel, (string?)null);
    }

    public void NotifyChannel(string channel, byte[]? payload)
    {
        if (payload == null)
        {
            NotifyChannel(channel, (string?)null);
            return;
        }

        NotifyChannel(channel, Encoding.UTF8.GetString(payload));
    }

    public void NotifyChannel(string channel, string? payload)
    {
        var sql = BuildNotifyCommand(channel, payload);
        TrackOperation("Connection.NotifyChannel", () => ExecuteControlCommand(sql));
    }

    public void AddNotificationListener(Action<ScratchBirdNotification> listener)
    {
        ArgumentNullException.ThrowIfNull(listener);
        EnsureNotificationBridge();
        lock (_notificationSync)
        {
            _notificationListeners ??= new HashSet<Action<ScratchBirdNotification>>();
            _notificationListeners.Add(listener);
        }
    }

    public bool RemoveNotificationListener(Action<ScratchBirdNotification> listener)
    {
        ArgumentNullException.ThrowIfNull(listener);
        EnsureNotificationBridge();
        lock (_notificationSync)
        {
            return _notificationListeners != null && _notificationListeners.Remove(listener);
        }
    }

    public ScratchBirdNotification? GetNotification()
    {
        EnsureNotificationBridge();
        lock (_notificationSync)
        {
            if (_notificationQueue == null || _notificationQueue.Count == 0)
            {
                return null;
            }

            return CloneNotification(_notificationQueue.Dequeue());
        }
    }

    public IReadOnlyList<ScratchBirdNotification> GetNotifications()
    {
        EnsureNotificationBridge();
        lock (_notificationSync)
        {
            if (_notificationQueue == null || _notificationQueue.Count == 0)
            {
                return Array.Empty<ScratchBirdNotification>();
            }

            var drained = new List<ScratchBirdNotification>(_notificationQueue.Count);
            while (_notificationQueue.Count > 0)
            {
                drained.Add(CloneNotification(_notificationQueue.Dequeue()));
            }
            return drained;
        }
    }

    public void ClearNotifications()
    {
        EnsureNotificationBridge();
        lock (_notificationSync)
        {
            _notificationQueue?.Clear();
        }
    }

    internal void AcceptNotification(
        uint processId,
        string channel,
        byte[] payload,
        char? changeType,
        ulong? rowId)
    {
        var notification = new ScratchBirdNotification(
            processId,
            channel,
            CloneBytes(payload),
            changeType,
            rowId,
            DateTimeOffset.UtcNow);
        Action<ScratchBirdNotification>[] listeners;
        lock (_notificationSync)
        {
            (_notificationQueue ??= new Queue<ScratchBirdNotification>()).Enqueue(notification);
            listeners = _notificationListeners?.ToArray() ?? Array.Empty<Action<ScratchBirdNotification>>();
        }

        foreach (var listener in listeners)
        {
            try
            {
                listener(CloneNotification(notification));
            }
            catch
            {
                // Consumer listener exceptions must not break connection protocol handling.
            }
        }
    }

    public ResultSetSummary Call(
        string sql,
        IReadOnlyList<ScratchBirdParameter>? parameters = null,
        int commandTimeoutSeconds = 30,
        int fetchSize = 0)
    {
        return TrackOperation("Connection.Call", () =>
        {
            var normalized = SqlHelpers.NormalizeCallable(sql, NormalizeParameterList(parameters));
            var resultSets = ExecuteQueryMultiInternal(
                normalized.Sql,
                normalized.Parameters,
                commandTimeoutSeconds,
                fetchSize);
            var preferred = resultSets.LastOrDefault(set => set.Rows.Count > 0);
            if (preferred != null)
            {
                return preferred;
            }
            return resultSets.Count > 0
                ? resultSets[^1]
                : new ResultSetSummary(
                    Array.Empty<object?[]>(),
                    0,
                    Array.Empty<FieldSummary>(),
                    string.Empty,
                    0);
        });
    }

    public IReadOnlyList<ResultSetSummary> QueryMulti(
        string sql,
        IReadOnlyList<ScratchBirdParameter>? parameters = null,
        int commandTimeoutSeconds = 30,
        int fetchSize = 0)
    {
        return TrackOperation("Connection.QueryMulti", () =>
        {
            var normalized = SqlHelpers.Normalize(sql, NormalizeParameterList(parameters));
            if (normalized.Parameters.Count == 0)
            {
                var statements = SplitSqlStatements(normalized.Sql);
                if (statements.Count > 1)
                {
                    var expanded = new List<ResultSetSummary>();
                    foreach (var statement in statements)
                    {
                        if (string.IsNullOrWhiteSpace(statement))
                        {
                            continue;
                        }
                        expanded.AddRange(ExecuteQueryMultiInternal(
                            statement,
                            Array.Empty<ScratchBirdParameter>(),
                            commandTimeoutSeconds,
                            fetchSize));
                    }
                    return (IReadOnlyList<ResultSetSummary>)expanded;
                }
            }

            return ExecuteQueryMultiInternal(
                normalized.Sql,
                normalized.Parameters,
                commandTimeoutSeconds,
                fetchSize);
        });
    }

    public IReadOnlyList<ResultSetSummary> ExecuteMulti(
        string sql,
        IReadOnlyList<ScratchBirdParameter>? parameters = null,
        int commandTimeoutSeconds = 30,
        int fetchSize = 0)
    {
        return QueryMulti(sql, parameters, commandTimeoutSeconds, fetchSize);
    }

    public BatchSummary ExecuteBatch(
        string sql,
        IReadOnlyList<IReadOnlyList<ScratchBirdParameter>> batchParameters,
        int commandTimeoutSeconds = 30,
        int fetchSize = 0)
    {
        return TrackOperation("Connection.ExecuteBatch", () =>
        {
            ArgumentNullException.ThrowIfNull(batchParameters);
            if (batchParameters.Count == 0)
            {
                throw new ArgumentException("batch parameters are required", nameof(batchParameters));
            }

            var items = new List<BatchItemSummary>(batchParameters.Count);
            long totalRowCount = 0;
            for (var i = 0; i < batchParameters.Count; i++)
            {
                var currentParameters = batchParameters[i] ?? Array.Empty<ScratchBirdParameter>();
                var resultSets = QueryMulti(sql, currentParameters, commandTimeoutSeconds, fetchSize);
                long rowCount = 0;
                IReadOnlyList<FieldSummary> fields = Array.Empty<FieldSummary>();
                var command = string.Empty;
                long lastInsertId = 0;

                foreach (var set in resultSets)
                {
                    if (set.RowCount > 0)
                    {
                        rowCount += set.RowCount;
                    }
                    if (set.Fields.Count > 0)
                    {
                        fields = set.Fields;
                    }
                    if (!string.IsNullOrEmpty(set.Command))
                    {
                        command = set.Command;
                    }
                    if (set.LastInsertId != 0)
                    {
                        lastInsertId = set.LastInsertId;
                    }
                }

                if (rowCount > 0)
                {
                    totalRowCount += rowCount;
                }

                items.Add(new BatchItemSummary(i, rowCount, fields, command, lastInsertId));
            }

            return new BatchSummary(items, totalRowCount);
        });
    }

    public BatchSummary QueryBatch(
        string sql,
        IReadOnlyList<IReadOnlyList<ScratchBirdParameter>> batchParameters,
        int commandTimeoutSeconds = 30,
        int fetchSize = 0)
    {
        return ExecuteBatch(sql, batchParameters, commandTimeoutSeconds, fetchSize);
    }

    public IReadOnlyList<long> ExecuteWithGeneratedKeys(
        string sql,
        IReadOnlyList<ScratchBirdParameter>? parameters = null,
        int commandTimeoutSeconds = 30,
        int fetchSize = 0)
    {
        return TrackOperation("Connection.ExecuteWithGeneratedKeys", () =>
        {
            var resultSets = QueryMulti(sql, parameters, commandTimeoutSeconds, fetchSize);
            return (IReadOnlyList<long>)resultSets
                .Where(set => set.LastInsertId != 0)
                .Select(set => set.LastInsertId)
                .ToArray();
        });
    }

    public override System.Data.DataTable GetSchema()
    {
        return GetSchema("Tables");
    }

    public override System.Data.DataTable GetSchema(string collectionName)
    {
        return GetSchema(collectionName, null);
    }

    public override System.Data.DataTable GetSchema(string collectionName, string[]? restrictionValues)
    {
        return TrackOperation("Connection.GetSchema", () =>
        {
            if (_state != ConnectionState.Open)
            {
                throw new InvalidOperationException("Connection is not open");
            }

            var collectionKey = NormalizeCollectionName(collectionName);
            var table = QueryMetadataCollection(collectionKey, collectionName);
            return ShapeMetadataTable(table, collectionKey, restrictionValues, _config.MetadataExpandSchemaParents);
        });
    }

    public DdlEditorSchemaPayload GetDdlEditorSchemaPayload(string? schemaPattern = null, bool? expandSchemaParents = null)
    {
        return TrackOperation("Connection.GetDdlEditorSchemaPayload", () =>
        {
            if (_state != ConnectionState.Open)
            {
                throw new InvalidOperationException("Connection is not open");
            }

            var schemaTable = QueryMetadataCollection("schemas", "Schemas");
            var expand = expandSchemaParents ?? _config.MetadataExpandSchemaParents;
            return BuildDdlEditorSchemaPayload(schemaTable, schemaPattern, expand);
        });
    }

    private DataTable QueryMetadataCollection(string collectionKey, string collectionName)
    {
        if (string.Equals(collectionKey, "catalogs", StringComparison.Ordinal))
        {
            return BuildCatalogsMetadataTable(collectionName, null);
        }

        var query = ResolveMetadataQuery(collectionKey, collectionName);
        using var cmd = CreateDbCommand();
        cmd.CommandText = query;
        using var reader = cmd.ExecuteReader();
        _ = reader.HasRows; // Prime row-description metadata before FieldCount/column enumeration.

        var table = new System.Data.DataTable(collectionName);
        for (var i = 0; i < reader.FieldCount; i++)
        {
            table.Columns.Add(reader.GetName(i), reader.GetFieldType(i));
        }

        while (reader.Read())
        {
            var row = table.NewRow();
            for (var i = 0; i < reader.FieldCount; i++)
            {
                row[i] = reader.IsDBNull(i) ? DBNull.Value : reader.GetValue(i);
            }
            table.Rows.Add(row);
        }

        return table;
    }

    private static string ResolveMetadataQuery(string collectionKey, string collectionName)
    {
        return collectionKey switch
        {
            "tables" => ScratchBirdMetadata.TablesQuery,
            "columns" => ScratchBirdMetadata.ColumnsQuery,
            "schemas" => ScratchBirdMetadata.SchemasQuery,
            "indexes" => ScratchBirdMetadata.IndexesQuery,
            "indexcolumns" => ScratchBirdMetadata.IndexColumnsQuery,
            "constraints" => ScratchBirdMetadata.ConstraintsQuery,
            "primarykeys" => ScratchBirdMetadata.PrimaryKeysQuery,
            "foreignkeys" => ScratchBirdMetadata.ForeignKeysQuery,
            "tableprivileges" => ScratchBirdMetadata.TablePrivilegesQuery,
            "columnprivileges" => ScratchBirdMetadata.ColumnPrivilegesQuery,
            "procedures" => ScratchBirdMetadata.ProceduresQuery,
            "functions" => ScratchBirdMetadata.FunctionsQuery,
            "routines" => ScratchBirdMetadata.RoutinesQuery,
            "typeinfo" => ScratchBirdMetadata.TypeInfoQuery,
            _ => throw new NotSupportedException($"Schema collection '{collectionName}' is not supported")
        };
    }

    private static string NormalizeCollectionName(string? collectionName)
    {
        return collectionName?.ToLowerInvariant() switch
        {
            "catalog" or "catalogs" => "catalogs",
            null or "" or "tables" => "tables",
            "columns" => "columns",
            "schemas" => "schemas",
            "indexes" => "indexes",
            "indexcolumns" or "index_columns" => "indexcolumns",
            "constraints" => "constraints",
            "primarykey" or "primarykeys" or "primary_keys" or "pk" => "primarykeys",
            "foreignkey" or "foreignkeys" or "foreign_keys" or "fk" => "foreignkeys",
            "tableprivileges" or "table_privileges" => "tableprivileges",
            "columnprivileges" or "column_privileges" => "columnprivileges",
            "procedures" => "procedures",
            "functions" => "functions",
            "routine" or "routines" => "routines",
            "typeinfo" or "type_info" or "types" => "typeinfo",
            _ => collectionName?.ToLowerInvariant() ?? string.Empty
        };
    }

    internal static string BuildPreparedTransactionSql(string verb, string globalTransactionId)
    {
        if (string.IsNullOrWhiteSpace(globalTransactionId))
        {
            throw new ScratchBirdSyntaxException("global transaction id is required", "42601");
        }

        var escaped = globalTransactionId.Trim().Replace("'", "''", StringComparison.Ordinal);
        return $"{verb} '{escaped}'";
    }

    internal static string NormalizeUuidText(string value, string label)
    {
        if (!Guid.TryParse(value, out var guid))
        {
            throw new ScratchBirdSyntaxException($"{label} must be a UUID", "42601");
        }

        return guid.ToString("D");
    }

    private DataTable BuildCatalogsMetadataTable(string collectionName, string?[]? restrictionValues)
    {
        var table = new DataTable(collectionName);
        table.Columns.Add("table_catalog", typeof(string));
        if (!string.IsNullOrWhiteSpace(_config.Database))
        {
            var row = table.NewRow();
            row["table_catalog"] = _config.Database;
            table.Rows.Add(row);
        }

        return ApplyRestrictionValuesForMetadata(table, "catalogs", restrictionValues);
    }

    internal static DataTable ShapeMetadataTable(
        DataTable table,
        string collectionKey,
        string?[]? restrictionValues,
        bool expandSchemaParents)
    {
        var shaped = FilterCollectionFamilyForMetadata(table, collectionKey);
        if (expandSchemaParents && string.Equals(collectionKey, "schemas", StringComparison.Ordinal))
        {
            shaped = ExpandSchemaParentsForMetadata(shaped);
        }

        return ApplyRestrictionValuesForMetadata(shaped, collectionKey, restrictionValues);
    }

    internal static DataTable FilterCollectionFamilyForMetadata(DataTable table, string collectionKey)
    {
        return collectionKey switch
        {
            "primarykeys" => FilterMetadataRowsByExpectedText(table, new[] { "constraint_type", "CONSTRAINT_TYPE" }, new[] { "primary key", "primary" }),
            "foreignkeys" => FilterMetadataRowsByExpectedText(table, new[] { "constraint_type", "CONSTRAINT_TYPE" }, new[] { "foreign key", "foreign" }),
            "procedures" => FilterMetadataRowsByExpectedText(table, new[] { "routine_type", "ROUTINE_TYPE" }, new[] { "procedure" }),
            "functions" => FilterMetadataRowsByExpectedText(table, new[] { "routine_type", "ROUTINE_TYPE" }, new[] { "function" }),
            _ => table
        };
    }

    internal static DdlEditorSchemaPayload BuildDdlEditorSchemaPayload(
        DataTable schemaTable,
        string? schemaPattern,
        bool expandSchemaParents)
    {
        ArgumentNullException.ThrowIfNull(schemaTable);

        var schemaColumn = ResolveColumnName(schemaTable, "schema_name", "table_schema", "table_schem");
        if (schemaColumn == null)
        {
            return new DdlEditorSchemaPayload(
                schemaPattern,
                expandSchemaParents,
                Array.Empty<string>(),
                Array.Empty<DdlEditorSchemaNode>());
        }

        var normalizedPattern = string.IsNullOrWhiteSpace(schemaPattern) ? null : schemaPattern.Trim();
        var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (DataRow row in schemaTable.Rows)
        {
            if (row[schemaColumn] == DBNull.Value || row[schemaColumn] == null)
            {
                continue;
            }

            var normalized = NormalizeSchemaPath(row[schemaColumn]!.ToString());
            if (normalized == null)
            {
                continue;
            }

            if (expandSchemaParents)
            {
                AppendSchemaParents(paths, normalized);
            }
            else
            {
                paths.Add(normalized);
            }
        }

        var schemaPaths = paths
            .Where(path => normalizedPattern == null || MatchesRestriction(path, normalizedPattern))
            .OrderBy(path => path, StringComparer.OrdinalIgnoreCase)
            .ToArray();

        var schemaTree = BuildSchemaTree(schemaPaths);
        return new DdlEditorSchemaPayload(normalizedPattern, expandSchemaParents, schemaPaths, schemaTree);
    }

    private static IReadOnlyList<DdlEditorSchemaNode> BuildSchemaTree(IEnumerable<string> schemaPaths)
    {
        var roots = new List<MutableSchemaNode>();
        var index = new Dictionary<string, MutableSchemaNode>(StringComparer.OrdinalIgnoreCase);
        foreach (var schemaPath in schemaPaths)
        {
            var parts = SplitSchemaPath(schemaPath);
            if (parts.Length == 0)
            {
                continue;
            }

            MutableSchemaNode? parent = null;
            var currentPath = new StringBuilder();
            foreach (var part in parts)
            {
                if (currentPath.Length > 0)
                {
                    currentPath.Append('.');
                }
                currentPath.Append(part);

                var fullPath = currentPath.ToString();
                if (!index.TryGetValue(fullPath, out var node))
                {
                    node = new MutableSchemaNode(part, fullPath);
                    index[fullPath] = node;
                    if (parent == null)
                    {
                        roots.Add(node);
                    }
                    else
                    {
                        parent.Children.Add(node);
                    }
                }

                parent = node;
            }

            if (parent != null)
            {
                parent.IsTerminal = true;
            }
        }

        return roots
            .OrderBy(node => node.Name, StringComparer.OrdinalIgnoreCase)
            .Select(ToImmutableSchemaNode)
            .ToArray();
    }

    private static DdlEditorSchemaNode ToImmutableSchemaNode(MutableSchemaNode node)
    {
        var children = node.Children
            .OrderBy(child => child.Name, StringComparer.OrdinalIgnoreCase)
            .Select(ToImmutableSchemaNode)
            .ToArray();
        return new DdlEditorSchemaNode(node.Name, node.FullPath, node.IsTerminal, children);
    }

    internal static DataTable ExpandSchemaParentsForMetadata(DataTable table)
    {
        var schemaColumn = ResolveColumnName(table, "schema_name", "table_schema", "table_schem");
        if (schemaColumn == null)
        {
            return table;
        }

        var schemaNames = table.Rows.Cast<DataRow>()
            .Select(row => row[schemaColumn] == DBNull.Value ? null : row[schemaColumn]?.ToString())
            .Where(name => !string.IsNullOrWhiteSpace(name))
            .Select(name => name!.Trim())
            .ToHashSet(StringComparer.OrdinalIgnoreCase);

        if (schemaNames.Count == 0)
        {
            return table;
        }

        var expanded = new HashSet<string>(schemaNames, StringComparer.OrdinalIgnoreCase);
        foreach (var schemaName in schemaNames)
        {
            AppendSchemaParents(expanded, schemaName);
        }

        if (expanded.Count == schemaNames.Count)
        {
            return table;
        }

        var existingRows = table.Rows.Cast<DataRow>()
            .Where(row => row[schemaColumn] != DBNull.Value)
            .GroupBy(row => row[schemaColumn]?.ToString() ?? string.Empty, StringComparer.OrdinalIgnoreCase)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.OrdinalIgnoreCase);

        var result = table.Clone();
        foreach (var schemaName in expanded.OrderBy(name => name, StringComparer.OrdinalIgnoreCase))
        {
            if (existingRows.TryGetValue(schemaName, out var existing))
            {
                result.ImportRow(existing);
                continue;
            }

            var synthetic = result.NewRow();
            synthetic[schemaColumn] = schemaName;
            result.Rows.Add(synthetic);
        }

        return result;
    }

    internal static DataTable ApplyRestrictionValuesForMetadata(
        DataTable table,
        string collectionKey,
        string?[]? restrictionValues)
    {
        if (restrictionValues == null || restrictionValues.Length == 0)
        {
            return table;
        }

        var restrictionColumns = ResolveRestrictionColumns(table, collectionKey);
        if (restrictionColumns.Count == 0)
        {
            return table;
        }

        var filtered = table.Clone();
        foreach (DataRow row in table.Rows)
        {
            if (!RowMatchesRestrictions(row, restrictionValues, restrictionColumns))
            {
                continue;
            }
            filtered.ImportRow(row);
        }

        return filtered;
    }

    private static DataTable FilterMetadataRowsByExpectedText(DataTable table, IReadOnlyList<string> aliases, IReadOnlyList<string> expectedValues)
    {
        var columnName = ResolveColumnName(table, aliases.ToArray());
        if (columnName == null)
        {
            return table;
        }

        var expected = new HashSet<string>(
            expectedValues.Select(value => value.Trim().ToLowerInvariant()),
            StringComparer.OrdinalIgnoreCase);
        var filtered = table.Clone();
        foreach (DataRow row in table.Rows)
        {
            var rawValue = row[columnName];
            if (rawValue == DBNull.Value || rawValue == null)
            {
                continue;
            }
            var normalized = rawValue.ToString()?.Trim().ToLowerInvariant();
            if (normalized == null || !expected.Contains(normalized))
            {
                continue;
            }
            filtered.ImportRow(row);
        }
        return filtered;
    }

    private static Dictionary<int, string> ResolveRestrictionColumns(DataTable table, string collectionKey)
    {
        var resolved = new Dictionary<int, string>();
        foreach (var (index, aliases) in RestrictionColumnAliases(collectionKey))
        {
            var column = ResolveColumnName(table, aliases);
            if (column != null)
            {
                resolved[index] = column;
            }
        }
        return resolved;
    }

    private static IEnumerable<(int index, string[] aliases)> RestrictionColumnAliases(string collectionKey)
    {
        return collectionKey switch
        {
            "tables" =>
            [
                (1, new[] { "table_schema", "TABLE_SCHEMA", "table_schem", "TABLE_SCHEM" }),
                (2, new[] { "table_name", "TABLE_NAME" }),
                (3, new[] { "table_type", "TABLE_TYPE" })
            ],
            "columns" =>
            [
                (1, new[] { "table_schema", "TABLE_SCHEMA", "table_schem", "TABLE_SCHEM" }),
                (2, new[] { "table_name", "TABLE_NAME" }),
                (3, new[] { "column_name", "COLUMN_NAME" })
            ],
            "indexes" =>
            [
                (1, new[] { "schema_name", "SCHEMA_NAME", "table_schema", "TABLE_SCHEMA", "table_schem", "TABLE_SCHEM" }),
                (2, new[] { "table_name", "TABLE_NAME", "table_id", "TABLE_ID" }),
                (3, new[] { "index_name", "INDEX_NAME", "index_id", "INDEX_ID" })
            ],
            "indexcolumns" =>
            [
                (1, new[] { "schema_name", "SCHEMA_NAME", "table_schema", "TABLE_SCHEMA", "table_schem", "TABLE_SCHEM" }),
                (2, new[] { "table_name", "TABLE_NAME", "table_id", "TABLE_ID" }),
                (3, new[] { "index_name", "INDEX_NAME", "index_id", "INDEX_ID" }),
                (4, new[] { "column_name", "COLUMN_NAME", "column_id", "COLUMN_ID" })
            ],
            "constraints" =>
            [
                (1, new[] { "schema_name", "SCHEMA_NAME", "table_schema", "TABLE_SCHEMA", "table_schem", "TABLE_SCHEM" }),
                (2, new[] { "table_name", "TABLE_NAME", "table_id", "TABLE_ID" }),
                (3, new[] { "constraint_name", "CONSTRAINT_NAME", "pk_name", "PK_NAME", "fk_name", "FK_NAME" })
            ],
            "primarykeys" =>
            [
                (1, new[] { "schema_name", "SCHEMA_NAME", "table_schema", "TABLE_SCHEMA", "table_schem", "TABLE_SCHEM" }),
                (2, new[] { "table_name", "TABLE_NAME", "table_id", "TABLE_ID" }),
                (3, new[] { "constraint_name", "CONSTRAINT_NAME", "pk_name", "PK_NAME" })
            ],
            "foreignkeys" =>
            [
                (1, new[] { "schema_name", "SCHEMA_NAME", "table_schema", "TABLE_SCHEMA", "table_schem", "TABLE_SCHEM" }),
                (2, new[] { "table_name", "TABLE_NAME", "table_id", "TABLE_ID" }),
                (3, new[] { "constraint_name", "CONSTRAINT_NAME", "fk_name", "FK_NAME" })
            ],
            "schemas" =>
            [
                (0, new[] { "schema_name", "SCHEMA_NAME", "table_schema", "TABLE_SCHEMA", "table_schem", "TABLE_SCHEM" }),
                (1, new[] { "schema_name", "SCHEMA_NAME", "table_schema", "TABLE_SCHEMA", "table_schem", "TABLE_SCHEM" })
            ],
            "catalogs" =>
            [
                (0, new[] { "table_catalog", "TABLE_CATALOG", "catalog_name", "CATALOG_NAME" })
            ],
            "tableprivileges" =>
            [
                (1, new[] { "schema_name", "SCHEMA_NAME", "table_schema", "TABLE_SCHEMA", "table_schem", "TABLE_SCHEM" }),
                (2, new[] { "table_name", "TABLE_NAME", "table_id", "TABLE_ID" }),
                (3, new[] { "grantor", "GRANTOR", "grantor_id", "GRANTOR_ID", "grantee", "GRANTEE", "grantee_id", "GRANTEE_ID" })
            ],
            "columnprivileges" =>
            [
                (1, new[] { "schema_name", "SCHEMA_NAME", "table_schema", "TABLE_SCHEMA", "table_schem", "TABLE_SCHEM" }),
                (2, new[] { "table_name", "TABLE_NAME", "table_id", "TABLE_ID" }),
                (3, new[] { "column_name", "COLUMN_NAME", "column_id", "COLUMN_ID" }),
                (4, new[] { "grantor", "GRANTOR", "grantor_id", "GRANTOR_ID", "grantee", "GRANTEE", "grantee_id", "GRANTEE_ID" })
            ],
            "procedures" =>
            [
                (1, new[] { "schema_name", "SCHEMA_NAME", "specific_schema", "SPECIFIC_SCHEMA", "routine_schema", "ROUTINE_SCHEMA", "schema_id", "SCHEMA_ID" }),
                (2, new[] { "procedure_name", "PROCEDURE_NAME", "routine_name", "ROUTINE_NAME", "specific_name", "SPECIFIC_NAME" })
            ],
            "functions" =>
            [
                (1, new[] { "schema_name", "SCHEMA_NAME", "specific_schema", "SPECIFIC_SCHEMA", "routine_schema", "ROUTINE_SCHEMA", "schema_id", "SCHEMA_ID" }),
                (2, new[] { "function_name", "FUNCTION_NAME", "routine_name", "ROUTINE_NAME", "specific_name", "SPECIFIC_NAME" })
            ],
            "routines" =>
            [
                (1, new[] { "schema_name", "SCHEMA_NAME", "specific_schema", "SPECIFIC_SCHEMA", "routine_schema", "ROUTINE_SCHEMA", "schema_id", "SCHEMA_ID" }),
                (2, new[] { "routine_name", "ROUTINE_NAME", "function_name", "FUNCTION_NAME", "procedure_name", "PROCEDURE_NAME", "specific_name", "SPECIFIC_NAME" })
            ],
            "typeinfo" =>
            [
                (0, new[] { "type_name", "TYPE_NAME", "data_type_name", "DATA_TYPE_NAME", "udt_name", "UDT_NAME", "data_type", "DATA_TYPE" })
            ],
            _ => Array.Empty<(int, string[])>()
        };
    }

    private static bool RowMatchesRestrictions(DataRow row, IReadOnlyList<string?> restrictionValues, IReadOnlyDictionary<int, string> restrictionColumns)
    {
        for (var i = 0; i < restrictionValues.Count; i++)
        {
            var restriction = restrictionValues[i];
            if (string.IsNullOrWhiteSpace(restriction))
            {
                continue;
            }
            if (!restrictionColumns.TryGetValue(i, out var columnName))
            {
                continue;
            }

            var rawValue = row[columnName];
            if (IsNullRestriction(restriction))
            {
                if (rawValue != DBNull.Value && rawValue != null)
                {
                    return false;
                }
                continue;
            }

            var value = rawValue == DBNull.Value ? string.Empty : rawValue?.ToString() ?? string.Empty;
            if (!MatchesRestriction(value, restriction))
            {
                return false;
            }
        }

        return true;
    }

    private static bool IsNullRestriction(string pattern)
    {
        return string.Equals(pattern.Trim(), "null", StringComparison.OrdinalIgnoreCase);
    }

    private static bool MatchesRestriction(string value, string pattern)
    {
        var regexBuilder = new StringBuilder("^");
        var escaped = false;
        foreach (var ch in pattern)
        {
            if (escaped)
            {
                regexBuilder.Append(Regex.Escape(ch.ToString()));
                escaped = false;
                continue;
            }

            if (ch == '\\')
            {
                escaped = true;
                continue;
            }

            if (ch == '%')
            {
                regexBuilder.Append(".*");
                continue;
            }
            if (ch == '_')
            {
                regexBuilder.Append('.');
                continue;
            }
            regexBuilder.Append(Regex.Escape(ch.ToString()));
        }

        if (escaped)
        {
            regexBuilder.Append(Regex.Escape("\\"));
        }

        regexBuilder.Append('$');

        var regexPattern = regexBuilder.ToString();
        return Regex.IsMatch(value, regexPattern, RegexOptions.CultureInvariant | RegexOptions.IgnoreCase);
    }

    private static void AppendSchemaParents(ISet<string> output, string schemaName)
    {
        var segments = SplitSchemaPath(schemaName);
        if (segments.Length == 0)
        {
            return;
        }

        var current = new StringBuilder();
        foreach (var segment in segments)
        {
            if (current.Length > 0)
            {
                current.Append('.');
            }
            current.Append(segment);
            output.Add(current.ToString());
        }
    }

    private static string? NormalizeSchemaPath(string? schemaName)
    {
        var segments = SplitSchemaPath(schemaName);
        return segments.Length == 0 ? null : string.Join(".", segments);
    }

    private static string[] SplitSchemaPath(string? schemaName)
    {
        if (string.IsNullOrWhiteSpace(schemaName))
        {
            return Array.Empty<string>();
        }

        return schemaName
            .Split('.', StringSplitOptions.None)
            .Select(segment => segment.Trim())
            .Where(segment => segment.Length > 0)
            .ToArray();
    }

    private static string? ResolveColumnName(DataTable table, params string[] aliases)
    {
        foreach (var alias in aliases)
        {
            foreach (DataColumn column in table.Columns)
            {
                if (string.Equals(column.ColumnName, alias, StringComparison.OrdinalIgnoreCase))
                {
                    return column.ColumnName;
                }
            }
        }

        return null;
    }

    private sealed class MutableSchemaNode
    {
        public MutableSchemaNode(string name, string fullPath)
        {
            Name = name;
            FullPath = fullPath;
            Children = new List<MutableSchemaNode>();
        }

        public string Name { get; }
        public string FullPath { get; }
        public bool IsTerminal { get; set; }
        public List<MutableSchemaNode> Children { get; }
    }

    private void ApplySchema()
    {
        if (string.IsNullOrWhiteSpace(_config.Schema) ||
            _config.Schema.Equals("public", StringComparison.OrdinalIgnoreCase))
        {
            return;
        }
        var statement = BuildSchemaStatement(_config.Schema);
        if (string.IsNullOrWhiteSpace(statement) || _client == null)
        {
            return;
        }
        var stream = _client.ExecuteQuery(statement);
        while (stream.ReadNextRow() != null)
        {
        }
    }

    private static string BuildSchemaStatement(string schema)
    {
        var trimmed = schema.Trim();
        if (string.IsNullOrEmpty(trimmed))
        {
            return string.Empty;
        }
        if (trimmed.Contains(',', StringComparison.Ordinal))
        {
            var parts = SplitTopLevel(trimmed, ',')
                .Select(part => part.Trim())
                .Where(part => !string.IsNullOrWhiteSpace(part))
                .Select(FormatSchemaPath)
                .ToArray();
            if (parts.Length == 0)
            {
                return string.Empty;
            }
            return $"SET SEARCH_PATH TO {string.Join(", ", parts)}";
        }
        return $"SET SCHEMA {FormatSchemaPath(trimmed)}";
    }

    private static string FormatSchemaPath(string schemaPath)
    {
        var segments = SplitTopLevel(schemaPath, '.')
            .Select(segment => segment.Trim())
            .Where(segment => !string.IsNullOrWhiteSpace(segment))
            .Select(NormalizeIdentifierSegment)
            .ToArray();
        if (segments.Length == 0)
        {
            return QuoteIdentifier(schemaPath.Trim());
        }
        return string.Join(".", segments);
    }

    private static string NormalizeIdentifierSegment(string segment)
    {
        if (segment.Length >= 2 && segment.StartsWith('"') && segment.EndsWith('"'))
        {
            return segment;
        }
        return QuoteIdentifier(segment);
    }

    private static string QuoteIdentifier(string name)
    {
        return $"\"{name.Replace("\"", "\"\"")}\"";
    }

    internal static PoolDiagnosticsSummary? MapPoolDiagnostics(ProtocolClientPool.PoolStats? stats)
    {
        if (!stats.HasValue)
        {
            return null;
        }

        var value = stats.Value;
        return new PoolDiagnosticsSummary(
            value.ActiveCount,
            value.IdleCount,
            value.MaxSize,
            value.MinSize,
            value.BorrowAttempts,
            value.Borrowed,
            value.Returned,
            value.Rejected,
            value.Evicted);
    }

    internal static QueryPlanSummary? CreateQueryPlanSummary(
        (uint Format, ulong PlanningTimeUs, ulong EstimatedRows, ulong EstimatedCost, byte[] Plan)? plan)
    {
        if (!plan.HasValue)
        {
            return null;
        }

        var value = plan.Value;
        return new QueryPlanSummary(
            value.Format,
            value.PlanningTimeUs,
            value.EstimatedRows,
            value.EstimatedCost,
            CloneBytes(value.Plan));
    }

    internal static SblrSummary? CreateSblrSummary((ulong Hash, uint Version, byte[] Bytecode)? sblr)
    {
        if (!sblr.HasValue)
        {
            return null;
        }

        var value = sblr.Value;
        return new SblrSummary(
            value.Hash,
            value.Version,
            CloneBytes(value.Bytecode));
    }

    private static CircuitBreakerSummary MapCircuitBreakerSummary(CircuitBreakerSnapshot snapshot)
    {
        return new CircuitBreakerSummary(
            snapshot.Enabled,
            snapshot.State,
            snapshot.FailureCount,
            snapshot.SuccessCount,
            snapshot.HalfOpenRequests,
            snapshot.FailureThreshold,
            snapshot.SuccessThreshold,
            snapshot.HalfOpenMaxRequests,
            snapshot.RecoveryTimeoutMs,
            snapshot.LastFailureUtc);
    }

    private static KeepaliveSummary MapKeepaliveSummary(KeepaliveSnapshot snapshot)
    {
        return new KeepaliveSummary(
            snapshot.Enabled,
            snapshot.IntervalMs,
            snapshot.MaxIdleBeforeCheckMs,
            snapshot.ValidationTimeoutMs,
            snapshot.LastActivityUtc,
            snapshot.LastValidationUtc,
            snapshot.LastIdleDurationMs,
            snapshot.ValidationAttempts,
            snapshot.ValidationSuccesses,
            snapshot.ValidationFailures);
    }

    private static PipelineSummary MapPipelineSummary(PipelineSnapshot snapshot)
    {
        return new PipelineSummary(
            snapshot.Enabled,
            snapshot.MaxInFlight,
            snapshot.InFlight,
            snapshot.TotalAccepted,
            snapshot.TotalRejected,
            snapshot.TotalCompleted,
            snapshot.TotalFailed);
    }

    private static LeakSummary MapLeakSummary(LeakSnapshot snapshot)
    {
        return new LeakSummary(
            snapshot.Enabled,
            snapshot.ActiveCheckout,
            snapshot.ThresholdMs,
            snapshot.CheckoutUtc,
            snapshot.LastCheckinUtc,
            snapshot.CurrentHeldDurationMs,
            snapshot.LastHeldDurationMs,
            snapshot.MaxHeldDurationMs,
            snapshot.PotentialLeakCount,
            snapshot.Checkouts,
            snapshot.Checkins,
            snapshot.CheckoutStackTrace);
    }

    private ProtocolClient EnsureNotificationBridge()
    {
        lock (_notificationSync)
        {
            _notificationBridgeRequested = true;
        }

        var client = EnsureConnectedClient();
        InstallNotificationBridgeIfNeeded(client);
        return client;
    }

    private void InstallNotificationBridgeIfNeeded(ProtocolClient client)
    {
        lock (_notificationSync)
        {
            if (!_notificationBridgeRequested || ReferenceEquals(_notificationBridgeClient, client))
            {
                return;
            }

            client.OnNotification(AcceptNotification);
            _notificationBridgeClient = client;
        }
    }

    private static ScratchBirdNotification CloneNotification(ScratchBirdNotification notification)
    {
        return notification with { Payload = CloneBytes(notification.Payload) };
    }

    internal static string NormalizeNotificationChannel(string? channel)
    {
        if (channel == null)
        {
            throw new ArgumentException("Notification channel cannot be null", nameof(channel));
        }

        var normalized = channel.Trim();
        if (normalized.Length == 0)
        {
            throw new ArgumentException("Notification channel cannot be empty", nameof(channel));
        }

        if (normalized.IndexOf('\0') >= 0)
        {
            throw new ArgumentException("Notification channel cannot contain NUL bytes", nameof(channel));
        }

        return normalized;
    }

    internal static string BuildNotifyCommand(string channel, string? payload)
    {
        var normalizedChannel = NormalizeNotificationChannel(channel);
        var sql = $"NOTIFY {QuoteIdentifier(normalizedChannel)}";
        if (payload == null)
        {
            return sql;
        }

        if (payload.IndexOf('\0') >= 0)
        {
            throw new ArgumentException("Notification payload cannot contain NUL bytes", nameof(payload));
        }

        return $"{sql}, {QuoteSqlLiteral(payload)}";
    }

    private static byte[] CloneBytes(byte[]? value)
    {
        if (value == null || value.Length == 0)
        {
            return Array.Empty<byte>();
        }

        return (byte[])value.Clone();
    }

    private void ExecuteControlCommand(string sql)
    {
        _ = ExecuteQueryMultiInternal(sql, Array.Empty<ScratchBirdParameter>(), commandTimeoutSeconds: 30, fetchSize: 0);
    }

    private static string QuoteSqlLiteral(string value)
    {
        return $"'{value.Replace("'", "''", StringComparison.Ordinal)}'";
    }

    private static IReadOnlyList<string> SplitTopLevel(string value, char delimiter)
    {
        var tokens = new List<string>();
        if (string.IsNullOrWhiteSpace(value))
        {
            return tokens;
        }

        var sb = new StringBuilder();
        var inDouble = false;
        for (var i = 0; i < value.Length; i++)
        {
            var c = value[i];
            if (c == '"')
            {
                sb.Append(c);
                if (inDouble && i + 1 < value.Length && value[i + 1] == '"')
                {
                    sb.Append('"');
                    i++;
                    continue;
                }
                inDouble = !inDouble;
                continue;
            }
            if (!inDouble && c == delimiter)
            {
                tokens.Add(sb.ToString());
                sb.Clear();
                continue;
            }
            sb.Append(c);
        }
        tokens.Add(sb.ToString());
        return tokens;
    }

    private static IReadOnlyList<string> SplitSqlStatements(string sql) =>
        SqlStatementSplitter.Split(sql);

    private IReadOnlyList<ResultSetSummary> ExecuteQueryMultiInternal(
        string sql,
        IReadOnlyList<ScratchBirdParameter> parameters,
        int commandTimeoutSeconds,
        int fetchSize)
    {
        var client = EnsureConnectedClient();
        var timeoutMs = commandTimeoutSeconds > 0 ? checked(commandTimeoutSeconds * 1000) : 0;
        var maxRows = fetchSize > 0 ? fetchSize : _config.DefaultFetchSize;
        return client.ExecuteQueryMulti(sql, parameters, timeoutMs, maxRows);
    }

    private static TelemetryOptions BuildTelemetryOptions(ScratchBirdConfig config)
    {
        return new TelemetryOptions(
            EnableTracing: config.TelemetryEnableTracing,
            EnableMetrics: config.TelemetryEnableMetrics,
            EnableSlowOperationLog: config.TelemetryEnableSlowOperationLog,
            SlowOperationThresholdMs: config.TelemetrySlowOperationThresholdMs,
            SlowOperationMaxEntries: config.TelemetrySlowOperationMaxEntries,
            SampleRate: config.TelemetrySampleRate,
            SanitizeStatements: config.TelemetrySanitizeStatements);
    }

    private static CircuitBreakerOptions BuildCircuitBreakerOptions(ScratchBirdConfig config)
    {
        return new CircuitBreakerOptions(
            FailureThreshold: config.CircuitBreakerFailureThreshold,
            RecoveryTimeoutMs: config.CircuitBreakerRecoveryTimeoutMs,
            SuccessThreshold: config.CircuitBreakerSuccessThreshold,
            HalfOpenMaxRequests: config.CircuitBreakerHalfOpenMaxRequests);
    }

    private static KeepaliveOptions BuildKeepaliveOptions(ScratchBirdConfig config)
    {
        return new KeepaliveOptions(
            IntervalMs: config.KeepaliveIntervalMs,
            MaxIdleBeforeCheckMs: config.KeepaliveMaxIdleBeforeCheckMs,
            ValidationTimeoutMs: config.KeepaliveValidationTimeoutMs);
    }

    private static PipelineOptions BuildPipelineOptions(ScratchBirdConfig config)
    {
        return new PipelineOptions(config.PipelineMaxInFlight);
    }

    private static LeakOptions BuildLeakOptions(ScratchBirdConfig config)
    {
        return new LeakOptions(
            ThresholdMs: config.LeakThresholdMs,
            CaptureStackTrace: config.LeakCaptureStackTrace);
    }

    private static IReadOnlyList<ScratchBirdParameter> NormalizeParameterList(IReadOnlyList<ScratchBirdParameter>? parameters)
    {
        return parameters ?? Array.Empty<ScratchBirdParameter>();
    }
}
