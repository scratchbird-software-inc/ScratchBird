// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Diagnostics;

namespace ScratchBird.Data;

internal readonly record struct LeakOptions(
    int ThresholdMs,
    bool CaptureStackTrace)
{
    public static LeakOptions Default { get; } = new(
        ThresholdMs: 30000,
        CaptureStackTrace: false);

    public bool Enabled => ThresholdMs > 0;

    public LeakOptions Normalize()
    {
        return new LeakOptions(Math.Max(0, ThresholdMs), CaptureStackTrace);
    }
}

internal readonly record struct LeakSnapshot(
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

internal sealed class LeakMonitor
{
    private readonly LeakOptions _options;
    private readonly Func<DateTimeOffset> _clock;
    private readonly object _sync = new();
    private DateTimeOffset? _checkoutUtc;
    private DateTimeOffset? _lastCheckinUtc;
    private long _lastHeldDurationMs;
    private long _maxHeldDurationMs;
    private long _checkouts;
    private long _checkins;
    private int _owningThreadId;
    private string? _checkoutStackTrace;

    public LeakMonitor()
        : this(LeakOptions.Default)
    {
    }

    public LeakMonitor(LeakOptions options, Func<DateTimeOffset>? clock = null)
    {
        _options = options.Normalize();
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
    }

    public void Checkout()
    {
        if (!_options.Enabled)
        {
            return;
        }

        lock (_sync)
        {
            if (_checkoutUtc.HasValue)
            {
                return;
            }

            _checkoutUtc = _clock();
            _owningThreadId = Environment.CurrentManagedThreadId;
            _checkoutStackTrace = _options.CaptureStackTrace
                ? new StackTrace(skipFrames: 1, fNeedFileInfo: false).ToString()
                : null;
            _checkouts++;
        }
    }

    public void Checkin()
    {
        if (!_options.Enabled)
        {
            return;
        }

        lock (_sync)
        {
            if (!_checkoutUtc.HasValue)
            {
                return;
            }

            var now = _clock();
            var heldMs = Math.Max(0L, (long)(now - _checkoutUtc.Value).TotalMilliseconds);
            _lastHeldDurationMs = heldMs;
            if (heldMs > _maxHeldDurationMs)
            {
                _maxHeldDurationMs = heldMs;
            }

            _checkoutUtc = null;
            _lastCheckinUtc = now;
            _checkoutStackTrace = null;
            _owningThreadId = 0;
            _checkins++;
        }
    }

    public LeakSnapshot Snapshot()
    {
        lock (_sync)
        {
            var now = _clock();
            var currentHeld = _checkoutUtc.HasValue
                ? Math.Max(0L, (long)(now - _checkoutUtc.Value).TotalMilliseconds)
                : 0L;
            var potential = _options.Enabled && _checkoutUtc.HasValue && currentHeld > _options.ThresholdMs
                ? 1L
                : 0L;
            return new LeakSnapshot(
                Enabled: _options.Enabled,
                ActiveCheckout: _checkoutUtc.HasValue,
                ThresholdMs: _options.ThresholdMs,
                CheckoutUtc: _checkoutUtc,
                LastCheckinUtc: _lastCheckinUtc,
                CurrentHeldDurationMs: currentHeld,
                LastHeldDurationMs: _lastHeldDurationMs,
                MaxHeldDurationMs: _maxHeldDurationMs,
                PotentialLeakCount: potential,
                Checkouts: _checkouts,
                Checkins: _checkins,
                CheckoutStackTrace: _checkoutStackTrace);
        }
    }
}
