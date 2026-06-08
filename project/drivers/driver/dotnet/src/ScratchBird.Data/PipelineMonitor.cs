// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

namespace ScratchBird.Data;

internal readonly record struct PipelineOptions(int MaxInFlight)
{
    public static PipelineOptions Default { get; } = new(MaxInFlight: 100);
    public bool Enabled => MaxInFlight > 0;

    public PipelineOptions Normalize()
    {
        return new PipelineOptions(Math.Max(0, MaxInFlight));
    }
}

internal readonly record struct PipelineSnapshot(
    bool Enabled,
    int MaxInFlight,
    int InFlight,
    long TotalAccepted,
    long TotalRejected,
    long TotalCompleted,
    long TotalFailed);

internal sealed class PipelineMonitor
{
    private readonly PipelineOptions _options;
    private readonly object _sync = new();
    private int _inFlight;
    private long _totalAccepted;
    private long _totalRejected;
    private long _totalCompleted;
    private long _totalFailed;

    public PipelineMonitor()
        : this(PipelineOptions.Default)
    {
    }

    public PipelineMonitor(PipelineOptions options)
    {
        _options = options.Normalize();
    }

    public bool TryAcquire()
    {
        if (!_options.Enabled)
        {
            return true;
        }

        lock (_sync)
        {
            if (_inFlight >= _options.MaxInFlight)
            {
                _totalRejected++;
                return false;
            }

            _inFlight++;
            _totalAccepted++;
            return true;
        }
    }

    public void Release(bool success)
    {
        if (!_options.Enabled)
        {
            return;
        }

        lock (_sync)
        {
            if (_inFlight > 0)
            {
                _inFlight--;
            }

            _totalCompleted++;
            if (!success)
            {
                _totalFailed++;
            }
        }
    }

    public PipelineSnapshot Snapshot()
    {
        lock (_sync)
        {
            return new PipelineSnapshot(
                Enabled: _options.Enabled,
                MaxInFlight: _options.MaxInFlight,
                InFlight: _inFlight,
                TotalAccepted: _totalAccepted,
                TotalRejected: _totalRejected,
                TotalCompleted: _totalCompleted,
                TotalFailed: _totalFailed);
        }
    }
}
