// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Collections.Concurrent;

namespace ScratchBird.Data;

public sealed class ScratchBirdQueryPipelineConfig
{
    public int MaxInFlight { get; set; } = 100;
    public bool AutoFlush { get; set; } = true;
    public int AutoFlushThreshold { get; set; } = 10;
    public int FlushTimeoutMs { get; set; } = 5000;

    internal ScratchBirdQueryPipelineConfig Normalize()
    {
        return new ScratchBirdQueryPipelineConfig
        {
            MaxInFlight = Math.Max(0, MaxInFlight),
            AutoFlush = AutoFlush,
            AutoFlushThreshold = Math.Max(1, AutoFlushThreshold),
            FlushTimeoutMs = Math.Max(1, FlushTimeoutMs)
        };
    }
}

public sealed record ScratchBirdPipelineBatchItem(
    string Sql,
    IReadOnlyList<ScratchBirdParameter>? Parameters = null,
    int CommandTimeoutSeconds = 30,
    int FetchSize = 0);

internal delegate Task<IReadOnlyList<ResultSetSummary>> ScratchBirdPipelineExecutor(
    string sql,
    IReadOnlyList<ScratchBirdParameter> parameters,
    int commandTimeoutSeconds,
    int fetchSize,
    CancellationToken cancellationToken);

public sealed class ScratchBirdQueryPipeline : IDisposable, IAsyncDisposable
{
    private readonly ScratchBirdConnection _connection;
    private readonly ScratchBirdQueryPipelineConfig _config;
    private readonly ScratchBirdPipelineExecutor _executor;
    private readonly ConcurrentQueue<PipelineRequest> _queue = new();
    private readonly SemaphoreSlim _flushSignal = new(0);
    private readonly CancellationTokenSource _shutdown = new();
    private readonly Task _worker;
    private int _pendingCount;
    private int _inFlightCount;
    private int _disposeState;

    public ScratchBirdQueryPipeline(ScratchBirdConnection connection, ScratchBirdQueryPipelineConfig? config = null)
        : this(connection, config ?? BuildDefaultConfig(connection), executor: null)
    {
    }

    internal ScratchBirdQueryPipeline(
        ScratchBirdConnection connection,
        ScratchBirdQueryPipelineConfig config,
        ScratchBirdPipelineExecutor? executor)
    {
        ArgumentNullException.ThrowIfNull(connection);
        ArgumentNullException.ThrowIfNull(config);

        _connection = connection;
        _config = config.Normalize();
        _executor = executor ?? ExecuteQueryAsync;
        _worker = Task.Run(RunLoopAsync);
    }

    public int PendingCount => Math.Max(0, Volatile.Read(ref _pendingCount));

    public int InFlightCount => Math.Max(0, Volatile.Read(ref _inFlightCount));

    public bool HasCapacity
    {
        get
        {
            var max = _config.MaxInFlight;
            if (max <= 0)
            {
                return true;
            }

            return PendingCount + InFlightCount < max;
        }
    }

    public Task<IReadOnlyList<ResultSetSummary>> QueueAsync(
        string sql,
        IReadOnlyList<ScratchBirdParameter>? parameters = null,
        int commandTimeoutSeconds = 30,
        int fetchSize = 0,
        CancellationToken cancellationToken = default)
    {
        ThrowIfDisposed();

        if (string.IsNullOrWhiteSpace(sql))
        {
            throw new ArgumentException("SQL is required", nameof(sql));
        }

        if (commandTimeoutSeconds < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(commandTimeoutSeconds), "commandTimeoutSeconds must be non-negative");
        }

        if (fetchSize < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(fetchSize), "fetchSize must be non-negative");
        }

        if (cancellationToken.IsCancellationRequested)
        {
            return Task.FromCanceled<IReadOnlyList<ResultSetSummary>>(cancellationToken);
        }

        if (!TryReserveCapacity(1, out var limitError))
        {
            return Task.FromException<IReadOnlyList<ResultSetSummary>>(limitError!);
        }

        var request = new PipelineRequest(
            sql,
            parameters ?? Array.Empty<ScratchBirdParameter>(),
            commandTimeoutSeconds,
            fetchSize,
            cancellationToken);
        _queue.Enqueue(request);

        if (_config.AutoFlush && PendingCount >= _config.AutoFlushThreshold)
        {
            Flush();
        }

        return request.Task;
    }

    public void Flush()
    {
        if (Volatile.Read(ref _disposeState) != 0)
        {
            return;
        }

        _flushSignal.Release();
    }

    public ScratchBirdQueryPipelineBatch CreateBatch()
    {
        ThrowIfDisposed();
        return new ScratchBirdQueryPipelineBatch(this);
    }

    public Task<IReadOnlyList<IReadOnlyList<ResultSetSummary>>> ExecuteBatchAsync(
        IReadOnlyList<ScratchBirdPipelineBatchItem> items,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(items);
        return QueueBatchInternalAsync(items, cancellationToken);
    }

    public void Dispose()
    {
        DisposeAsync().AsTask().GetAwaiter().GetResult();
    }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposeState, 1) != 0)
        {
            return;
        }

        _shutdown.Cancel();
        _flushSignal.Release();

        try
        {
            await _worker.ConfigureAwait(false);
        }
        catch
        {
            // Requests are failed/cancelled from RunLoopAsync teardown path.
        }

        _flushSignal.Dispose();
        _shutdown.Dispose();
    }

    private async Task RunLoopAsync()
    {
        var shutdownToken = _shutdown.Token;
        while (!shutdownToken.IsCancellationRequested)
        {
            try
            {
                _ = await _flushSignal.WaitAsync(_config.FlushTimeoutMs, shutdownToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                break;
            }

            while (_queue.TryDequeue(out var request))
            {
                Interlocked.Decrement(ref _pendingCount);

                if (request.CancellationToken.IsCancellationRequested)
                {
                    request.TrySetCanceled(request.CancellationToken);
                    continue;
                }

                Interlocked.Increment(ref _inFlightCount);
                try
                {
                    using var linkedCts = CancellationTokenSource.CreateLinkedTokenSource(shutdownToken, request.CancellationToken);
                    var result = await _executor(
                        request.Sql,
                        request.Parameters,
                        request.CommandTimeoutSeconds,
                        request.FetchSize,
                        linkedCts.Token).ConfigureAwait(false);
                    request.TrySetResult(result);
                }
                catch (OperationCanceledException) when (request.CancellationToken.IsCancellationRequested)
                {
                    request.TrySetCanceled(request.CancellationToken);
                }
                catch (OperationCanceledException) when (shutdownToken.IsCancellationRequested)
                {
                    request.TrySetException(new ObjectDisposedException(nameof(ScratchBirdQueryPipeline)));
                }
                catch (Exception ex)
                {
                    request.TrySetException(ex);
                }
                finally
                {
                    Interlocked.Decrement(ref _inFlightCount);
                }
            }
        }

        while (_queue.TryDequeue(out var pending))
        {
            Interlocked.Decrement(ref _pendingCount);
            if (pending.CancellationToken.IsCancellationRequested)
            {
                pending.TrySetCanceled(pending.CancellationToken);
            }
            else
            {
                pending.TrySetException(new ObjectDisposedException(nameof(ScratchBirdQueryPipeline)));
            }
        }
    }

    private Task<IReadOnlyList<IReadOnlyList<ResultSetSummary>>> QueueBatchInternalAsync(
        IReadOnlyList<ScratchBirdPipelineBatchItem> items,
        CancellationToken cancellationToken)
    {
        ThrowIfDisposed();
        cancellationToken.ThrowIfCancellationRequested();
        if (items.Count == 0)
        {
            return Task.FromResult<IReadOnlyList<IReadOnlyList<ResultSetSummary>>>(Array.Empty<IReadOnlyList<ResultSetSummary>>());
        }

        var normalized = new NormalizedBatchItem[items.Count];
        for (var i = 0; i < items.Count; i++)
        {
            normalized[i] = NormalizeBatchItem(items[i]);
        }

        if (!TryReserveCapacity(normalized.Length, out var limitError))
        {
            return Task.FromException<IReadOnlyList<IReadOnlyList<ResultSetSummary>>>(limitError!);
        }

        var tasks = new Task<IReadOnlyList<ResultSetSummary>>[normalized.Length];
        for (var i = 0; i < normalized.Length; i++)
        {
            var item = normalized[i];
            var request = new PipelineRequest(
                item.Sql,
                item.Parameters,
                item.CommandTimeoutSeconds,
                item.FetchSize,
                cancellationToken);
            tasks[i] = request.Task;
            _queue.Enqueue(request);
        }

        Flush();
        return AwaitBatchAsync(tasks, cancellationToken);
    }

    private static async Task<IReadOnlyList<IReadOnlyList<ResultSetSummary>>> AwaitBatchAsync(
        Task<IReadOnlyList<ResultSetSummary>>[] tasks,
        CancellationToken cancellationToken)
    {
        var results = await Task.WhenAll(tasks).WaitAsync(cancellationToken).ConfigureAwait(false);
        return results;
    }

    private bool TryReserveCapacity(int requestedSlots, out ScratchBirdLimitException? limitError)
    {
        if (requestedSlots <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(requestedSlots), "requestedSlots must be positive");
        }

        var max = _config.MaxInFlight;
        while (true)
        {
            var pending = Volatile.Read(ref _pendingCount);
            var inFlight = Volatile.Read(ref _inFlightCount);
            if (max > 0 && pending + inFlight + requestedSlots > max)
            {
                limitError = new ScratchBirdLimitException(
                    "Pipeline queue at capacity",
                    "54000",
                    $"pipeline_max_in_flight={max}");
                return false;
            }

            if (Interlocked.CompareExchange(ref _pendingCount, pending + requestedSlots, pending) == pending)
            {
                limitError = null;
                return true;
            }
        }
    }

    private Task<IReadOnlyList<ResultSetSummary>> ExecuteQueryAsync(
        string sql,
        IReadOnlyList<ScratchBirdParameter> parameters,
        int commandTimeoutSeconds,
        int fetchSize,
        CancellationToken cancellationToken)
    {
        return Task.Run(
            () => _connection.QueryMulti(sql, parameters, commandTimeoutSeconds, fetchSize),
            cancellationToken);
    }

    private static ScratchBirdQueryPipelineConfig BuildDefaultConfig(ScratchBirdConnection connection)
    {
        return new ScratchBirdQueryPipelineConfig
        {
            MaxInFlight = connection.Config.PipelineMaxInFlight,
            AutoFlush = connection.Config.PipelineAutoFlush,
            AutoFlushThreshold = connection.Config.PipelineAutoFlushThreshold,
            FlushTimeoutMs = connection.Config.PipelineFlushTimeoutMs
        };
    }

    private static NormalizedBatchItem NormalizeBatchItem(ScratchBirdPipelineBatchItem item)
    {
        ArgumentNullException.ThrowIfNull(item);

        if (string.IsNullOrWhiteSpace(item.Sql))
        {
            throw new ArgumentException("SQL is required", nameof(item.Sql));
        }

        if (item.CommandTimeoutSeconds < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(item.CommandTimeoutSeconds), "commandTimeoutSeconds must be non-negative");
        }

        if (item.FetchSize < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(item.FetchSize), "fetchSize must be non-negative");
        }

        return new NormalizedBatchItem(
            item.Sql,
            item.Parameters ?? Array.Empty<ScratchBirdParameter>(),
            item.CommandTimeoutSeconds,
            item.FetchSize);
    }

    private void ThrowIfDisposed()
    {
        if (Volatile.Read(ref _disposeState) != 0)
        {
            throw new ObjectDisposedException(nameof(ScratchBirdQueryPipeline));
        }
    }

    private sealed class PipelineRequest
    {
        private readonly TaskCompletionSource<IReadOnlyList<ResultSetSummary>> _completion =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public PipelineRequest(
            string sql,
            IReadOnlyList<ScratchBirdParameter> parameters,
            int commandTimeoutSeconds,
            int fetchSize,
            CancellationToken cancellationToken)
        {
            Sql = sql;
            Parameters = parameters;
            CommandTimeoutSeconds = commandTimeoutSeconds;
            FetchSize = fetchSize;
            CancellationToken = cancellationToken;
        }

        public string Sql { get; }
        public IReadOnlyList<ScratchBirdParameter> Parameters { get; }
        public int CommandTimeoutSeconds { get; }
        public int FetchSize { get; }
        public CancellationToken CancellationToken { get; }

        public Task<IReadOnlyList<ResultSetSummary>> Task => _completion.Task;

        public void TrySetResult(IReadOnlyList<ResultSetSummary> result)
        {
            _completion.TrySetResult(result);
        }

        public void TrySetException(Exception exception)
        {
            _completion.TrySetException(exception);
        }

        public void TrySetCanceled(CancellationToken cancellationToken)
        {
            _completion.TrySetCanceled(cancellationToken);
        }
    }

    private readonly record struct NormalizedBatchItem(
        string Sql,
        IReadOnlyList<ScratchBirdParameter> Parameters,
        int CommandTimeoutSeconds,
        int FetchSize);
}

public sealed class ScratchBirdQueryPipelineBatch
{
    private readonly ScratchBirdQueryPipeline _pipeline;
    private readonly List<BatchItem> _items = new();

    internal ScratchBirdQueryPipelineBatch(ScratchBirdQueryPipeline pipeline)
    {
        _pipeline = pipeline;
    }

    public int Count => _items.Count;

    public ScratchBirdQueryPipelineBatch Add(
        string sql,
        IReadOnlyList<ScratchBirdParameter>? parameters = null,
        int commandTimeoutSeconds = 30,
        int fetchSize = 0)
    {
        if (string.IsNullOrWhiteSpace(sql))
        {
            throw new ArgumentException("SQL is required", nameof(sql));
        }

        if (commandTimeoutSeconds < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(commandTimeoutSeconds), "commandTimeoutSeconds must be non-negative");
        }

        if (fetchSize < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(fetchSize), "fetchSize must be non-negative");
        }

        _items.Add(new BatchItem(
            sql,
            parameters ?? Array.Empty<ScratchBirdParameter>(),
            commandTimeoutSeconds,
            fetchSize));
        return this;
    }

    public async Task<IReadOnlyList<IReadOnlyList<ResultSetSummary>>> ExecuteAsync(CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (_items.Count == 0)
        {
            return Array.Empty<IReadOnlyList<ResultSetSummary>>();
        }

        var items = new ScratchBirdPipelineBatchItem[_items.Count];
        for (var i = 0; i < _items.Count; i++)
        {
            var item = _items[i];
            items[i] = new ScratchBirdPipelineBatchItem(
                item.Sql,
                item.Parameters,
                item.CommandTimeoutSeconds,
                item.FetchSize);
        }

        return await _pipeline.ExecuteBatchAsync(items, cancellationToken).ConfigureAwait(false);
    }

    private readonly record struct BatchItem(
        string Sql,
        IReadOnlyList<ScratchBirdParameter> Parameters,
        int CommandTimeoutSeconds,
        int FetchSize);
}
