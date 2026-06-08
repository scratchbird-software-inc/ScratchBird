// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Collections.Concurrent;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

namespace ScratchBird.Data;

internal readonly record struct TelemetryOptions(
    bool EnableTracing,
    bool EnableMetrics,
    bool EnableSlowOperationLog,
    int SlowOperationThresholdMs,
    int SlowOperationMaxEntries,
    double SampleRate,
    bool SanitizeStatements)
{
    public static TelemetryOptions Default { get; } = new(
        EnableTracing: true,
        EnableMetrics: true,
        EnableSlowOperationLog: true,
        SlowOperationThresholdMs: 1000,
        SlowOperationMaxEntries: 100,
        SampleRate: 1d,
        SanitizeStatements: true);

    public TelemetryOptions Normalize()
    {
        return new TelemetryOptions(
            EnableTracing,
            EnableMetrics,
            EnableSlowOperationLog,
            Math.Max(0, SlowOperationThresholdMs),
            Math.Max(0, SlowOperationMaxEntries),
            Math.Clamp(SampleRate, 0d, 1d),
            SanitizeStatements);
    }
}

internal sealed class TelemetryCollector
{
    private sealed class OperationCounters
    {
        public long Invocations;
        public long Successes;
        public long Failures;
        public long TotalDurationMs;
        public long MaxDurationMs;
    }

    private readonly TelemetryOptions _options;
    private readonly ConcurrentDictionary<string, OperationCounters> _operations = new(StringComparer.Ordinal);
    private readonly object _slowOperationsSync = new();
    private readonly Queue<SlowOperationSummary> _slowOperations = new();
    private long _histMs0To10;
    private long _histMs10To100;
    private long _histMs100To1000;
    private long _histMs1000To10000;
    private long _histMsOver10000;

    public TelemetryCollector()
        : this(TelemetryOptions.Default)
    {
    }

    public TelemetryCollector(TelemetryOptions options)
    {
        _options = options.Normalize();
    }

    public void Record(string operation, TimeSpan duration, bool success, string? statement = null)
    {
        if (string.IsNullOrWhiteSpace(operation) || !_options.EnableTracing)
        {
            return;
        }

        var durationMs = Math.Max(0, (long)duration.TotalMilliseconds);
        if (_options.SampleRate < 1d && Random.Shared.NextDouble() > _options.SampleRate)
        {
            return;
        }

        if (_options.EnableMetrics)
        {
            var counters = _operations.GetOrAdd(operation, _ => new OperationCounters());
            Interlocked.Increment(ref counters.Invocations);
            if (success)
            {
                Interlocked.Increment(ref counters.Successes);
            }
            else
            {
                Interlocked.Increment(ref counters.Failures);
            }

            Interlocked.Add(ref counters.TotalDurationMs, durationMs);
            UpdateMaxDuration(counters, durationMs);
            UpdateLatencyHistogram(durationMs);
        }

        if (_options.EnableSlowOperationLog && durationMs > _options.SlowOperationThresholdMs)
        {
            RecordSlowOperation(operation, durationMs, success, statement);
        }
    }

    public ConnectionTelemetrySummary Snapshot()
    {
        var operationSummaries = new List<OperationTelemetrySummary>(_operations.Count);
        foreach (var entry in _operations)
        {
            var counters = entry.Value;
            var invocations = Interlocked.Read(ref counters.Invocations);
            var successes = Interlocked.Read(ref counters.Successes);
            var failures = Interlocked.Read(ref counters.Failures);
            var totalDurationMs = Interlocked.Read(ref counters.TotalDurationMs);
            var maxDurationMs = Interlocked.Read(ref counters.MaxDurationMs);
            var average = invocations == 0 ? 0d : (double)totalDurationMs / invocations;
            operationSummaries.Add(new OperationTelemetrySummary(
                entry.Key,
                invocations,
                successes,
                failures,
                totalDurationMs,
                maxDurationMs,
                average));
        }

        operationSummaries.Sort((left, right) => string.Compare(left.Operation, right.Operation, StringComparison.Ordinal));
        var totalInvocations = operationSummaries.Sum(item => item.Invocations);
        var totalSuccesses = operationSummaries.Sum(item => item.Successes);
        var totalFailures = operationSummaries.Sum(item => item.Failures);
        return new ConnectionTelemetrySummary(
            DateTimeOffset.UtcNow,
            totalInvocations,
            totalSuccesses,
            totalFailures,
            operationSummaries);
    }

    public IReadOnlyList<SlowOperationSummary> GetSlowOperations()
    {
        lock (_slowOperationsSync)
        {
            return _slowOperations.ToArray();
        }
    }

    public string ExportPrometheusMetrics()
    {
        var snapshot = Snapshot();
        var bucket0To10 = Interlocked.Read(ref _histMs0To10);
        var bucket10To100 = Interlocked.Read(ref _histMs10To100);
        var bucket100To1000 = Interlocked.Read(ref _histMs100To1000);
        var bucket1000To10000 = Interlocked.Read(ref _histMs1000To10000);
        var bucketOver10000 = Interlocked.Read(ref _histMsOver10000);

        var cumulative10 = bucket0To10;
        var cumulative100 = cumulative10 + bucket10To100;
        var cumulative1000 = cumulative100 + bucket100To1000;
        var cumulative10000 = cumulative1000 + bucket1000To10000;
        var cumulativeInf = cumulative10000 + bucketOver10000;

        int slowOperationCount;
        lock (_slowOperationsSync)
        {
            slowOperationCount = _slowOperations.Count;
        }

        var sb = new StringBuilder();
        sb.AppendLine("# HELP scratchbird_operations_total Total number of recorded driver operations");
        sb.AppendLine("# TYPE scratchbird_operations_total counter");
        sb.Append("scratchbird_operations_total ")
            .Append(snapshot.TotalInvocations.ToString(CultureInfo.InvariantCulture))
            .AppendLine();

        sb.AppendLine("# HELP scratchbird_operations_success_total Total successful operations");
        sb.AppendLine("# TYPE scratchbird_operations_success_total counter");
        sb.Append("scratchbird_operations_success_total ")
            .Append(snapshot.TotalSuccesses.ToString(CultureInfo.InvariantCulture))
            .AppendLine();

        sb.AppendLine("# HELP scratchbird_operations_failure_total Total failed operations");
        sb.AppendLine("# TYPE scratchbird_operations_failure_total counter");
        sb.Append("scratchbird_operations_failure_total ")
            .Append(snapshot.TotalFailures.ToString(CultureInfo.InvariantCulture))
            .AppendLine();

        sb.AppendLine("# HELP scratchbird_operation_duration_ms Operation latency histogram");
        sb.AppendLine("# TYPE scratchbird_operation_duration_ms histogram");
        sb.Append("scratchbird_operation_duration_ms_bucket{le=\"10\"} ")
            .Append(cumulative10.ToString(CultureInfo.InvariantCulture))
            .AppendLine();
        sb.Append("scratchbird_operation_duration_ms_bucket{le=\"100\"} ")
            .Append(cumulative100.ToString(CultureInfo.InvariantCulture))
            .AppendLine();
        sb.Append("scratchbird_operation_duration_ms_bucket{le=\"1000\"} ")
            .Append(cumulative1000.ToString(CultureInfo.InvariantCulture))
            .AppendLine();
        sb.Append("scratchbird_operation_duration_ms_bucket{le=\"10000\"} ")
            .Append(cumulative10000.ToString(CultureInfo.InvariantCulture))
            .AppendLine();
        sb.Append("scratchbird_operation_duration_ms_bucket{le=\"+Inf\"} ")
            .Append(cumulativeInf.ToString(CultureInfo.InvariantCulture))
            .AppendLine();
        sb.Append("scratchbird_operation_duration_ms_count ")
            .Append(snapshot.TotalInvocations.ToString(CultureInfo.InvariantCulture))
            .AppendLine();

        sb.AppendLine("# HELP scratchbird_slow_operations_queued Number of retained slow operations");
        sb.AppendLine("# TYPE scratchbird_slow_operations_queued gauge");
        sb.Append("scratchbird_slow_operations_queued ")
            .Append(slowOperationCount.ToString(CultureInfo.InvariantCulture))
            .AppendLine();

        sb.AppendLine("# HELP scratchbird_operation_invocations_total Operation invocations by operation name");
        sb.AppendLine("# TYPE scratchbird_operation_invocations_total counter");
        foreach (var operation in snapshot.Operations)
        {
            sb.Append("scratchbird_operation_invocations_total{operation=\"")
                .Append(EscapePrometheusLabel(operation.Operation))
                .Append("\"} ")
                .Append(operation.Invocations.ToString(CultureInfo.InvariantCulture))
                .AppendLine();
        }

        return sb.ToString();
    }

    public void Reset()
    {
        _operations.Clear();
        lock (_slowOperationsSync)
        {
            _slowOperations.Clear();
        }

        Interlocked.Exchange(ref _histMs0To10, 0);
        Interlocked.Exchange(ref _histMs10To100, 0);
        Interlocked.Exchange(ref _histMs100To1000, 0);
        Interlocked.Exchange(ref _histMs1000To10000, 0);
        Interlocked.Exchange(ref _histMsOver10000, 0);
    }

    private static void UpdateMaxDuration(OperationCounters counters, long candidate)
    {
        while (true)
        {
            var current = Interlocked.Read(ref counters.MaxDurationMs);
            if (candidate <= current)
            {
                return;
            }

            if (Interlocked.CompareExchange(ref counters.MaxDurationMs, candidate, current) == current)
            {
                return;
            }
        }
    }

    private void UpdateLatencyHistogram(long durationMs)
    {
        if (durationMs <= 10)
        {
            Interlocked.Increment(ref _histMs0To10);
        }
        else if (durationMs <= 100)
        {
            Interlocked.Increment(ref _histMs10To100);
        }
        else if (durationMs <= 1000)
        {
            Interlocked.Increment(ref _histMs100To1000);
        }
        else if (durationMs <= 10000)
        {
            Interlocked.Increment(ref _histMs1000To10000);
        }
        else
        {
            Interlocked.Increment(ref _histMsOver10000);
        }
    }

    private void RecordSlowOperation(string operation, long durationMs, bool success, string? statement)
    {
        if (_options.SlowOperationMaxEntries <= 0)
        {
            return;
        }

        var summary = new SlowOperationSummary(
            operation,
            durationMs,
            success,
            DateTimeOffset.UtcNow,
            NormalizeStatement(statement));
        lock (_slowOperationsSync)
        {
            _slowOperations.Enqueue(summary);
            while (_slowOperations.Count > _options.SlowOperationMaxEntries)
            {
                _slowOperations.Dequeue();
            }
        }
    }

    private static string EscapePrometheusLabel(string value)
    {
        if (string.IsNullOrEmpty(value))
        {
            return string.Empty;
        }

        return value.Replace("\\", "\\\\", StringComparison.Ordinal)
            .Replace("\"", "\\\"", StringComparison.Ordinal)
            .Replace("\n", "\\n", StringComparison.Ordinal);
    }

    private string? NormalizeStatement(string? statement)
    {
        if (string.IsNullOrWhiteSpace(statement))
        {
            return null;
        }

        var normalized = statement.Trim();
        if (_options.SanitizeStatements)
        {
            normalized = SanitizeStatement(normalized);
        }

        const int maxStatementChars = 512;
        if (normalized.Length > maxStatementChars)
        {
            normalized = normalized[..maxStatementChars];
        }

        return normalized;
    }

    internal static string SanitizeStatement(string statement)
    {
        if (string.IsNullOrEmpty(statement))
        {
            return statement;
        }

        return Regex.Replace(statement, "'(?:''|[^'])*'", "'?'", RegexOptions.CultureInvariant);
    }
}
