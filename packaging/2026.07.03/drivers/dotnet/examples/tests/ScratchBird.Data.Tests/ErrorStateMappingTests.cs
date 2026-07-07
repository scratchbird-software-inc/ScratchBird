// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using ScratchBird.Data;
using Xunit;

namespace ScratchBird.Data.Tests;

public class ErrorStateMappingTests
{
    [Fact]
    public void ExactSqlStateMapsToSpecificErrorType()
    {
        var ex = ScratchBirdSqlStateMapper.Create("exact", "42P01", null, null);
        Assert.IsType<ScratchBirdSyntaxException>(ex);
        Assert.Equal("42P01", ex.SqlState);
    }

    [Fact]
    public void SqlStateClassPrefixMapsToCategory()
    {
        var ex = ScratchBirdSqlStateMapper.Create("class", "22000", null, null);
        Assert.IsType<ScratchBirdDataException>(ex);
        Assert.Equal("22000", ex.SqlState);
    }

    [Fact]
    public void UnrecognizedStateFallsBackToBaseException()
    {
        var ex = ScratchBirdSqlStateMapper.Create("base", "ZZ123", null, null);
        Assert.IsType<ScratchBirdException>(ex);
        Assert.Equal("ZZ123", ex.SqlState);
    }

    [Fact]
    public void EmptySqlStateFallsBackToBaseException()
    {
        var ex = ScratchBirdSqlStateMapper.Create("empty", string.Empty, null, null);
        Assert.IsType<ScratchBirdException>(ex);
    }

    [Fact]
    public void SqlStateClassUsesExpectedConnectionCategory()
    {
        var ex = ScratchBirdSqlStateMapper.Create("class", "08012", null, null);
        Assert.IsType<ScratchBirdConnectionException>(ex);
        Assert.Equal("08012", ex.SqlState);
    }

    [Fact]
    public void RetryScopeClassifiesStatementAndReconnectBoundaries()
    {
        Assert.Equal(ScratchBirdRetryScope.Statement, ScratchBirdSqlStateMapper.RetryScopeForSqlState("40001"));
        Assert.Equal(ScratchBirdRetryScope.Statement, ScratchBirdSqlStateMapper.RetryScopeForSqlState("40P01"));
        Assert.Equal(ScratchBirdRetryScope.Reconnect, ScratchBirdSqlStateMapper.RetryScopeForSqlState("08006"));
        Assert.Equal(ScratchBirdRetryScope.None, ScratchBirdSqlStateMapper.RetryScopeForSqlState("57014"));
        Assert.Equal(ScratchBirdRetryScope.None, ScratchBirdSqlStateMapper.RetryScopeForSqlState(null));
    }

    [Fact]
    public void IsRetryableSqlStateOnlyAllowsFreshBoundaryRetries()
    {
        Assert.True(ScratchBirdSqlStateMapper.IsRetryableSqlState("40001"));
        Assert.True(ScratchBirdSqlStateMapper.IsRetryableSqlState("08003"));
        Assert.False(ScratchBirdSqlStateMapper.IsRetryableSqlState("57014"));
        Assert.False(ScratchBirdSqlStateMapper.IsRetryableSqlState(string.Empty));
    }
}
