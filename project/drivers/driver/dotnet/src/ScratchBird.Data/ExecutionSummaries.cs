// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Data;

namespace ScratchBird.Data;

public sealed record FieldSummary(string Name, uint TypeOid, ushort Format, bool Nullable);

public sealed record ResultSetSummary(
    IReadOnlyList<object?[]> Rows,
    long RowCount,
    IReadOnlyList<FieldSummary> Fields,
    string Command,
    long LastInsertId);

public sealed record BatchItemSummary(
    int Index,
    long RowCount,
    IReadOnlyList<FieldSummary> Fields,
    string Command,
    long LastInsertId);

public sealed record BatchSummary(
    IReadOnlyList<BatchItemSummary> Items,
    long TotalRowCount);

public sealed record PoolDiagnosticsSummary(
    int ActiveCount,
    int IdleCount,
    int MaxSize,
    int MinSize,
    long BorrowAttempts,
    long Borrowed,
    long Returned,
    long Rejected,
    long Evicted);

public sealed record QueryPlanSummary(
    uint Format,
    ulong PlanningTimeUs,
    ulong EstimatedRows,
    ulong EstimatedCost,
    byte[] Plan);

public sealed record SblrSummary(
    ulong Hash,
    uint Version,
    byte[] Bytecode);

public enum CircuitBreakerState
{
    Closed,
    Open,
    HalfOpen
}

public sealed record CircuitBreakerSummary(
    bool Enabled,
    CircuitBreakerState State,
    int FailureCount,
    int SuccessCount,
    int HalfOpenRequests,
    int FailureThreshold,
    int SuccessThreshold,
    int HalfOpenMaxRequests,
    int RecoveryTimeoutMs,
    DateTimeOffset? LastFailureUtc);

public sealed record KeepaliveSummary(
    bool Enabled,
    int IntervalMs,
    int MaxIdleBeforeCheckMs,
    int ValidationTimeoutMs,
    DateTimeOffset LastActivityUtc,
    DateTimeOffset? LastValidationUtc,
    long LastIdleDurationMs,
    long ValidationAttempts,
    long ValidationSuccesses,
    long ValidationFailures);

public sealed record PipelineSummary(
    bool Enabled,
    int MaxInFlight,
    int InFlight,
    long TotalAccepted,
    long TotalRejected,
    long TotalCompleted,
    long TotalFailed);

public sealed record LeakSummary(
    bool Enabled,
    bool ActiveCheckout,
    long ThresholdMs,
    DateTimeOffset? CheckoutUtc,
    DateTimeOffset? LastCheckinUtc,
    long CurrentHeldDurationMs,
    long LastHeldDurationMs,
    long MaxHeldDurationMs,
    long PotentialLeakCount,
    long Checkouts,
    long Checkins,
    string? CheckoutStackTrace);

public sealed record ScratchBirdNotification(
    uint ProcessId,
    string Channel,
    byte[] Payload,
    char? ChangeType,
    ulong? RowId,
    DateTimeOffset ReceivedUtc);

public sealed record OperationTelemetrySummary(
    string Operation,
    long Invocations,
    long Successes,
    long Failures,
    long TotalDurationMs,
    long MaxDurationMs,
    double AverageDurationMs);

public sealed record ConnectionTelemetrySummary(
    DateTimeOffset CapturedUtc,
    long TotalInvocations,
    long TotalSuccesses,
    long TotalFailures,
    IReadOnlyList<OperationTelemetrySummary> Operations);

public sealed record SlowOperationSummary(
    string Operation,
    long DurationMs,
    bool Success,
    DateTimeOffset CapturedUtc,
    string? Statement);

public sealed record ConnectionDiagnosticsSummary(
    DateTimeOffset CapturedUtc,
    ConnectionState State,
    bool IsHealthy,
    string FrontDoorMode,
    string Protocol,
    string Host,
    int Port,
    string Database,
    bool Pooling,
    PoolDiagnosticsSummary? Pool,
    QueryPlanSummary? LastPlan,
    SblrSummary? LastSblr,
    CircuitBreakerSummary CircuitBreaker,
    KeepaliveSummary Keepalive,
    PipelineSummary Pipeline,
    LeakSummary LeakDetection);
