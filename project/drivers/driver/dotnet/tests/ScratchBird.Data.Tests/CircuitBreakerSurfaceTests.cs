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

public class CircuitBreakerSurfaceTests
{
    [Fact]
    public void CircuitBreakerDisabledByDefault_DoesNotOpenAfterFailures()
    {
        using var connection = new ScratchBirdConnection("Host=localhost;Port=3092;Database=testdb");

        for (var i = 0; i < 6; i++)
        {
            Assert.Throws<InvalidOperationException>(() => connection.Listen("alerts"));
        }

        var summary = connection.GetCircuitBreakerSummary();
        Assert.False(summary.Enabled);
        Assert.Equal(CircuitBreakerState.Closed, summary.State);
    }

    [Fact]
    public void CircuitBreakerOpensAfterConfiguredFailureThreshold()
    {
        using var connection = new ScratchBirdConnection(
            "Host=localhost;Port=3092;Database=testdb;" +
            "cb_failure_threshold=2;cb_recovery_timeout_ms=60000;cb_success_threshold=1;cb_half_open_max_requests=1");

        Assert.Throws<InvalidOperationException>(() => connection.Listen("alerts"));
        Assert.Throws<InvalidOperationException>(() => connection.Listen("alerts"));

        var summary = connection.GetCircuitBreakerSummary();
        Assert.True(summary.Enabled);
        Assert.Equal(CircuitBreakerState.Open, summary.State);
        Assert.True(summary.LastFailureUtc.HasValue);

        var ex = Assert.Throws<ScratchBirdConnectionException>(() => connection.Listen("alerts"));
        Assert.Equal("08006", ex.SqlState);
    }
}
