// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

namespace ScratchBird.Data;

internal readonly record struct CircuitBreakerOptions(
    int FailureThreshold,
    int RecoveryTimeoutMs,
    int SuccessThreshold,
    int HalfOpenMaxRequests)
{
    public static CircuitBreakerOptions Default { get; } = new(
        FailureThreshold: 0,
        RecoveryTimeoutMs: 30000,
        SuccessThreshold: 2,
        HalfOpenMaxRequests: 1);

    public bool Enabled => FailureThreshold > 0;

    public CircuitBreakerOptions Normalize()
    {
        return new CircuitBreakerOptions(
            Math.Max(0, FailureThreshold),
            Math.Max(1, RecoveryTimeoutMs),
            Math.Max(1, SuccessThreshold),
            Math.Max(1, HalfOpenMaxRequests));
    }
}

internal readonly record struct CircuitBreakerSnapshot(
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

internal sealed class CircuitBreaker
{
    private readonly CircuitBreakerOptions _options;
    private readonly object _sync = new();
    private CircuitBreakerState _state = CircuitBreakerState.Closed;
    private int _failureCount;
    private int _successCount;
    private int _halfOpenRequests;
    private DateTimeOffset? _lastFailureUtc;

    public CircuitBreaker()
        : this(CircuitBreakerOptions.Default)
    {
    }

    public CircuitBreaker(CircuitBreakerOptions options)
    {
        _options = options.Normalize();
    }

    public bool AllowRequest()
    {
        if (!_options.Enabled)
        {
            return true;
        }

        lock (_sync)
        {
            if (_state == CircuitBreakerState.Open)
            {
                if (!_lastFailureUtc.HasValue
                    || (DateTimeOffset.UtcNow - _lastFailureUtc.Value).TotalMilliseconds < _options.RecoveryTimeoutMs)
                {
                    return false;
                }

                _state = CircuitBreakerState.HalfOpen;
                _successCount = 0;
                _halfOpenRequests = 0;
            }

            if (_state == CircuitBreakerState.HalfOpen)
            {
                if (_halfOpenRequests >= _options.HalfOpenMaxRequests)
                {
                    return false;
                }

                _halfOpenRequests++;
            }

            return true;
        }
    }

    public void RecordSuccess()
    {
        if (!_options.Enabled)
        {
            return;
        }

        lock (_sync)
        {
            if (_state == CircuitBreakerState.HalfOpen)
            {
                if (_halfOpenRequests > 0)
                {
                    _halfOpenRequests--;
                }

                _successCount++;
                if (_successCount >= _options.SuccessThreshold)
                {
                    ResetClosed();
                }
                return;
            }

            if (_state == CircuitBreakerState.Closed)
            {
                _failureCount = 0;
            }
        }
    }

    public void RecordFailure()
    {
        if (!_options.Enabled)
        {
            return;
        }

        lock (_sync)
        {
            _lastFailureUtc = DateTimeOffset.UtcNow;
            if (_state == CircuitBreakerState.HalfOpen)
            {
                if (_halfOpenRequests > 0)
                {
                    _halfOpenRequests--;
                }

                OpenBreaker();
                return;
            }

            if (_state == CircuitBreakerState.Closed)
            {
                _failureCount++;
                if (_failureCount >= _options.FailureThreshold)
                {
                    OpenBreaker();
                }
            }
        }
    }

    public CircuitBreakerSnapshot Snapshot()
    {
        lock (_sync)
        {
            return new CircuitBreakerSnapshot(
                Enabled: _options.Enabled,
                State: _state,
                FailureCount: _failureCount,
                SuccessCount: _successCount,
                HalfOpenRequests: _halfOpenRequests,
                FailureThreshold: _options.FailureThreshold,
                SuccessThreshold: _options.SuccessThreshold,
                HalfOpenMaxRequests: _options.HalfOpenMaxRequests,
                RecoveryTimeoutMs: _options.RecoveryTimeoutMs,
                LastFailureUtc: _lastFailureUtc);
        }
    }

    private void OpenBreaker()
    {
        _state = CircuitBreakerState.Open;
        _successCount = 0;
        _halfOpenRequests = 0;
    }

    private void ResetClosed()
    {
        _state = CircuitBreakerState.Closed;
        _failureCount = 0;
        _successCount = 0;
        _halfOpenRequests = 0;
        _lastFailureUtc = null;
    }
}
