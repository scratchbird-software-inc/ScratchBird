// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System;
using ScratchBird.Data;
using Xunit;

namespace ScratchBird.Data.Tests;

public class LeakSurfaceTests
{
    [Fact]
    public void LeakMonitor_TracksPotentialLeakWhenHeldOverThreshold()
    {
        var now = DateTimeOffset.UtcNow;
        var monitor = new LeakMonitor(
            new LeakOptions(ThresholdMs: 10, CaptureStackTrace: false),
            () => now);

        monitor.Checkout();
        now = now.AddMilliseconds(25);

        var active = monitor.Snapshot();
        Assert.True(active.ActiveCheckout);
        Assert.Equal(1, active.PotentialLeakCount);

        monitor.Checkin();
        var closed = monitor.Snapshot();
        Assert.False(closed.ActiveCheckout);
        Assert.Equal(0, closed.PotentialLeakCount);
        Assert.True(closed.LastHeldDurationMs >= 25);
        Assert.Equal(1, closed.Checkouts);
        Assert.Equal(1, closed.Checkins);
    }

    [Fact]
    public void LeakMonitor_CapturesStackWhenEnabled()
    {
        var monitor = new LeakMonitor(new LeakOptions(ThresholdMs: 100, CaptureStackTrace: true));
        monitor.Checkout();
        var summary = monitor.Snapshot();
        Assert.True(summary.ActiveCheckout);
        Assert.False(string.IsNullOrWhiteSpace(summary.CheckoutStackTrace));
    }

    [Fact]
    public void ConnectionSurface_ExposesLeakSummary()
    {
        using var connection = new ScratchBirdConnection(
            "Host=localhost;Port=3092;Database=main;leak_threshold_ms=12345;leak_capture_stack=1");

        var summary = connection.GetLeakSummary();
        Assert.True(summary.Enabled);
        Assert.Equal(12345, summary.ThresholdMs);
        Assert.False(summary.ActiveCheckout);
    }
}
