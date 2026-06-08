// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

namespace ScratchBird.Data;

internal readonly record struct KeepaliveOptions(
    int IntervalMs,
    int MaxIdleBeforeCheckMs,
    int ValidationTimeoutMs)
{
    public static KeepaliveOptions Default { get; } = new(
        IntervalMs: 120000,
        MaxIdleBeforeCheckMs: 600000,
        ValidationTimeoutMs: 5000);

    public bool Enabled => IntervalMs > 0 && MaxIdleBeforeCheckMs > 0 && ValidationTimeoutMs > 0;

    public KeepaliveOptions Normalize()
    {
        return new KeepaliveOptions(
            Math.Max(0, IntervalMs),
            Math.Max(0, MaxIdleBeforeCheckMs),
            Math.Max(0, ValidationTimeoutMs));
    }
}

internal readonly record struct KeepaliveSnapshot(
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

internal sealed class KeepaliveMonitor
{
    private readonly KeepaliveOptions _options;
    private readonly Func<DateTimeOffset> _clock;
    private readonly object _sync = new();
    private DateTimeOffset _lastActivityUtc;
    private DateTimeOffset? _lastValidationUtc;
    private long _lastIdleDurationMs;
    private long _validationAttempts;
    private long _validationSuccesses;
    private long _validationFailures;

    public KeepaliveMonitor()
        : this(KeepaliveOptions.Default)
    {
    }

    public KeepaliveMonitor(KeepaliveOptions options, Func<DateTimeOffset>? clock = null)
    {
        _options = options.Normalize();
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
        _lastActivityUtc = _clock();
    }

    public void MarkActivity()
    {
        lock (_sync)
        {
            _lastActivityUtc = _clock();
            _lastIdleDurationMs = 0;
        }
    }

    public bool ValidateIfNeeded(Func<bool> validator)
    {
        if (!_options.Enabled)
        {
            return true;
        }

        ArgumentNullException.ThrowIfNull(validator);

        DateTimeOffset now;
        lock (_sync)
        {
            now = _clock();
            var idleMs = Math.Max(0L, (long)(now - _lastActivityUtc).TotalMilliseconds);
            _lastIdleDurationMs = idleMs;
            if (idleMs <= _options.MaxIdleBeforeCheckMs)
            {
                return true;
            }

            if (_lastValidationUtc.HasValue
                && (now - _lastValidationUtc.Value).TotalMilliseconds < _options.IntervalMs)
            {
                return true;
            }

            _lastValidationUtc = now;
            _validationAttempts++;
        }

        bool validated;
        try
        {
            validated = validator();
        }
        catch
        {
            RecordValidationResult(false);
            throw;
        }

        RecordValidationResult(validated);
        return validated;
    }

    public KeepaliveSnapshot Snapshot()
    {
        lock (_sync)
        {
            var idleMs = Math.Max(0L, (long)(_clock() - _lastActivityUtc).TotalMilliseconds);
            return new KeepaliveSnapshot(
                Enabled: _options.Enabled,
                IntervalMs: _options.IntervalMs,
                MaxIdleBeforeCheckMs: _options.MaxIdleBeforeCheckMs,
                ValidationTimeoutMs: _options.ValidationTimeoutMs,
                LastActivityUtc: _lastActivityUtc,
                LastValidationUtc: _lastValidationUtc,
                LastIdleDurationMs: idleMs,
                ValidationAttempts: _validationAttempts,
                ValidationSuccesses: _validationSuccesses,
                ValidationFailures: _validationFailures);
        }
    }

    private void RecordValidationResult(bool success)
    {
        lock (_sync)
        {
            if (success)
            {
                _validationSuccesses++;
                _lastActivityUtc = _clock();
                _lastIdleDurationMs = 0;
            }
            else
            {
                _validationFailures++;
            }
        }
    }
}
