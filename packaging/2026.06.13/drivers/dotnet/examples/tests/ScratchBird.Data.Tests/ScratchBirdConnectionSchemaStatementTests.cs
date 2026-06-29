// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Reflection;
using ScratchBird.Data;
using Xunit;

namespace ScratchBird.Data.Tests;

public class ScratchBirdConnectionSchemaStatementTests
{
    [Fact]
    public void BuildSchemaStatementSupportsRecursiveSchemaPath()
    {
        Assert.Equal("SET SCHEMA \"public\".\"examples\"",
            BuildSchemaStatement("public.examples"));
    }

    [Fact]
    public void BuildSchemaStatementSupportsRecursiveSearchPathList()
    {
        Assert.Equal("SET SEARCH_PATH TO \"public\".\"examples\", \"compat\".\"mysql\"",
            BuildSchemaStatement("public.examples, compat.mysql"));
    }

    [Fact]
    public void BuildSchemaStatementPreservesQuotedSegments()
    {
        Assert.Equal("SET SCHEMA \"Public\".\"Examples\"",
            BuildSchemaStatement("\"Public\".\"Examples\""));
    }

    private static string BuildSchemaStatement(string input)
    {
        var method = typeof(ScratchBirdConnection).GetMethod(
            "BuildSchemaStatement",
            BindingFlags.NonPublic | BindingFlags.Static);
        Assert.NotNull(method);
        return (string)method!.Invoke(null, new object[] { input })!;
    }
}
