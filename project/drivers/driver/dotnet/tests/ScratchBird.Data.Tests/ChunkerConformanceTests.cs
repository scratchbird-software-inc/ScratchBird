// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using ScratchBird.Data;
using Xunit;

namespace ScratchBird.Data.Tests;

/// <summary>
/// Cross-driver statement-chunker conformance. Loads the shared oracle fixture
/// (tests/conformance/drivers/chunker_conformance/cases.json) and asserts the
/// .NET driver's <see cref="SqlStatementSplitter"/> reproduces every
/// <c>expected</c> statement list exactly. Mirrors the Python reference verifier
/// (verify_python_reference.py).
/// </summary>
public class ChunkerConformanceTests
{
    public static IEnumerable<object[]> Cases()
    {
        foreach (var c in LoadCases())
        {
            yield return new object[] { c };
        }
    }

    public static IEnumerable<object[]> ChainCases()
    {
        foreach (var c in LoadChainCases())
        {
            yield return new object[] { c };
        }
    }

    [Theory]
    [MemberData(nameof(Cases))]
    public void SplitterMatchesFixture(ChunkerCase fixtureCase)
    {
        var actual = SqlStatementSplitter.Split(fixtureCase.Input).ToArray();
        Assert.Equal(fixtureCase.Expected, actual);
    }

    [Fact]
    public void AllFixtureCasesPass()
    {
        var cases = LoadCases();
        Assert.Equal(10, cases.Count);
        foreach (var c in cases)
        {
            var actual = SqlStatementSplitter.Split(c.Input).ToArray();
            Assert.Equal(c.Expected, actual);
        }
    }

    [Theory]
    [MemberData(nameof(ChainCases))]
    public void ChainSplitterMatchesFixture(ChainChunkerCase fixtureCase)
    {
        var actual = SqlStatementSplitter.SplitChain(fixtureCase.Input)
            .Select(statement => new[]
            {
                statement.ScriptName,
                statement.StatementIndex.ToString(System.Globalization.CultureInfo.InvariantCulture),
                statement.Sql
            })
            .ToArray();
        Assert.Equal(fixtureCase.Expected, actual);
    }

    [Fact]
    public void AllChainFixtureCasesPass()
    {
        var cases = LoadChainCases();
        Assert.Equal(4, cases.Count);
        foreach (var c in cases)
        {
            var actual = SqlStatementSplitter.SplitChain(c.Input)
                .Select(statement => new[]
                {
                    statement.ScriptName,
                    statement.StatementIndex.ToString(System.Globalization.CultureInfo.InvariantCulture),
                    statement.Sql
                })
                .ToArray();
            Assert.Equal(c.Expected, actual);
        }
    }

    [Fact]
    public void AlreadyNormalizedProceduralCreateIsAtomic()
    {
        const string sql = """
CREATE TRIGGER trig_items_ai
AFTER INSERT
ON TABLE trig_items
FOR EACH ROW
AS
BEGIN
  INSERT INTO trig_audit (audit_id, event_kind) VALUES (1, 'INSERT');
END
""";
        var actual = SqlStatementSplitter.Split(sql);
        Assert.Single(actual);
        Assert.Equal(sql.Trim(), actual[0]);
    }

    private static IReadOnlyList<ChunkerCase> LoadCases()
    {
        var path = LocateCasesJson();
        using var stream = File.OpenRead(path);
        using var doc = JsonDocument.Parse(stream);
        var cases = new List<ChunkerCase>();
        foreach (var element in doc.RootElement.GetProperty("cases").EnumerateArray())
        {
            var name = element.GetProperty("name").GetString()!;
            var input = element.GetProperty("input").GetString()!;
            var expected = element.GetProperty("expected")
                .EnumerateArray()
                .Select(e => e.GetString()!)
                .ToArray();
            cases.Add(new ChunkerCase(name, input, expected));
        }
        return cases;
    }

    private static string LocateCasesJson()
    {
        const string relative =
            "project/tests/conformance/drivers/chunker_conformance/cases.json";
        return LocateFixture(relative);
    }

    private static IReadOnlyList<ChainChunkerCase> LoadChainCases()
    {
        var path = LocateChainCasesJson();
        using var stream = File.OpenRead(path);
        using var doc = JsonDocument.Parse(stream);
        var cases = new List<ChainChunkerCase>();
        foreach (var element in doc.RootElement.GetProperty("cases").EnumerateArray())
        {
            var name = element.GetProperty("name").GetString()!;
            var input = element.GetProperty("input").GetString()!;
            var expected = element.GetProperty("expected")
                .EnumerateArray()
                .Select(row => row.EnumerateArray()
                    .Select(e => e.ValueKind == JsonValueKind.Number
                        ? e.GetInt32().ToString(System.Globalization.CultureInfo.InvariantCulture)
                        : e.GetString()!)
                    .ToArray())
                .ToArray();
            cases.Add(new ChainChunkerCase(name, input, expected));
        }
        return cases;
    }

    private static string LocateChainCasesJson()
    {
        const string relative =
            "project/tests/conformance/drivers/chunker_conformance/chain_cases.json";
        return LocateFixture(relative);
    }

    private static string LocateFixture(string relative)
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null)
        {
            var candidate = Path.Combine(dir.FullName, relative);
            if (File.Exists(candidate))
            {
                return candidate;
            }
            dir = dir.Parent;
        }
        throw new FileNotFoundException(
            $"Could not locate chunker conformance fixture '{relative}' " +
            $"walking up from '{AppContext.BaseDirectory}'.");
    }
}

public sealed record ChunkerCase(string Name, string Input, string[] Expected)
{
    public override string ToString() => Name;
}

public sealed record ChainChunkerCase(string Name, string Input, string[][] Expected)
{
    public override string ToString() => Name;
}
