// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Data;
using System.Linq;
using System.Reflection;
using System;
using ScratchBird.Data;
using Xunit;

namespace ScratchBird.Data.Tests;

public class ScratchBirdConnectionMetadataShapingTests
{
    [Fact]
    public void BuildDdlEditorSchemaPayload_ExpandsParentsAndBuildsTree()
    {
        var table = CreateSchemasTable(
            "users.alice.dev",
            "users.bob",
            "analytics.prod",
            "sys");

        var payload = ScratchBirdConnection.BuildDdlEditorSchemaPayload(
            table,
            schemaPattern: null,
            expandSchemaParents: true);

        Assert.True(payload.ExpandSchemaParents);
        Assert.Equal(
            new[]
            {
                "analytics",
                "analytics.prod",
                "sys",
                "users",
                "users.alice",
                "users.alice.dev",
                "users.bob"
            },
            payload.SchemaPaths);

        var usersRoot = Assert.Single(payload.SchemaTree.Where(node => node.Name == "users"));
        Assert.True(usersRoot.IsTerminal);

        var usersAlice = Assert.Single(usersRoot.Children.Where(node => node.Name == "alice"));
        Assert.True(usersAlice.IsTerminal);
        Assert.Single(usersAlice.Children);
        Assert.Equal("users.alice.dev", usersAlice.Children[0].FullPath);
        Assert.True(usersAlice.Children[0].IsTerminal);
    }

    [Fact]
    public void ApplyRestrictionValuesForMetadata_TreatsEscapedWildcardCharactersAsLiterals()
    {
        var table = CreateSchemasTable(
            "users_qa",
            "usersxqa",
            "acct%prod",
            "acctXprod");

        var underscoreFiltered = ScratchBirdConnection.ApplyRestrictionValuesForMetadata(
            table,
            "schemas",
            new[] { null, @"users\_qa" });
        Assert.Equal(new[] { "users_qa" }, ReadSchemaNames(underscoreFiltered));

        var percentFiltered = ScratchBirdConnection.ApplyRestrictionValuesForMetadata(
            table,
            "schemas",
            new[] { null, @"acct\%prod" });
        Assert.Equal(new[] { "acct%prod" }, ReadSchemaNames(percentFiltered));
    }

    [Fact]
    public void ExpandSchemaParentsForMetadataAddsMissingParentsAndPreservesDistinctBranches()
    {
        var table = CreateSchemasTable(
            "users.alice.dev",
            "users.bob.dev",
            "analytics.prod",
            "sys");

        var expanded = ScratchBirdConnection.ExpandSchemaParentsForMetadata(table);

        Assert.Equal(
            new[]
            {
                "analytics",
                "analytics.prod",
                "sys",
                "users",
                "users.alice",
                "users.alice.dev",
                "users.bob",
                "users.bob.dev"
            },
            ReadSchemaNames(expanded));
    }

    [Fact]
    public void ShapeMetadataTableExpansionStillRespectsSchemaRestrictionPattern()
    {
        var table = CreateSchemasTable(
            "users.alice.dev",
            "users.bob.dev",
            "analytics.prod");

        var shaped = ScratchBirdConnection.ShapeMetadataTable(
            table,
            "schemas",
            new[] { null, "users.%" },
            expandSchemaParents: true);

        Assert.Equal(
            new[]
            {
                "users.alice",
                "users.alice.dev",
                "users.bob",
                "users.bob.dev"
            },
            ReadSchemaNames(shaped));
    }

    [Fact]
    public void ApplyRestrictionValuesForMetadataFiltersTablesBySchemaAndName()
    {
        var table = new DataTable("Tables");
        table.Columns.Add("table_schema", typeof(string));
        table.Columns.Add("table_name", typeof(string));
        table.Columns.Add("table_type", typeof(string));
        table.Rows.Add("users.alice", "orders", "BASE TABLE");
        table.Rows.Add("users.bob", "orders", "BASE TABLE");
        table.Rows.Add("sys", "users", "SYSTEM TABLE");

        var filtered = ScratchBirdConnection.ApplyRestrictionValuesForMetadata(
            table,
            "tables",
            new[] { null, "users.%", "orders", "BASE TABLE" });

        Assert.Equal(2, filtered.Rows.Count);
        Assert.All(
            filtered.Rows.Cast<DataRow>(),
            row => Assert.Equal("orders", row["table_name"]?.ToString()));
    }

    [Fact]
    public void ApplyRestrictionValuesForMetadataFiltersColumnsByColumnPattern()
    {
        var table = new DataTable("Columns");
        table.Columns.Add("table_schema", typeof(string));
        table.Columns.Add("table_name", typeof(string));
        table.Columns.Add("column_name", typeof(string));
        table.Rows.Add("users.alice", "orders", "id");
        table.Rows.Add("users.alice", "orders", "note");
        table.Rows.Add("users.alice", "orders", "net_total");

        var filtered = ScratchBirdConnection.ApplyRestrictionValuesForMetadata(
            table,
            "columns",
            new[] { null, "users.alice", "orders", "n_t%" });

        Assert.Equal(2, filtered.Rows.Count);
        var names = filtered.Rows.Cast<DataRow>()
            .Select(row => row["column_name"]?.ToString())
            .ToArray();
        Assert.Contains("note", names);
        Assert.Contains("net_total", names);
    }

    [Fact]
    public void ApplyRestrictionValuesForMetadataFiltersCatalogRows()
    {
        var table = new DataTable("Catalogs");
        table.Columns.Add("TABLE_CATALOG", typeof(string));
        table.Rows.Add("main");
        table.Rows.Add("analytics");

        var filtered = ScratchBirdConnection.ApplyRestrictionValuesForMetadata(
            table,
            "catalogs",
            new[] { "main" });

        Assert.Single(filtered.Rows);
        Assert.Equal("main", filtered.Rows[0]["TABLE_CATALOG"]?.ToString());
    }

    [Fact]
    public void ApplyRestrictionValuesForMetadataFiltersProceduresBySchemaAndName()
    {
        var table = new DataTable("Procedures");
        table.Columns.Add("schema_name", typeof(string));
        table.Columns.Add("procedure_name", typeof(string));
        table.Rows.Add("users.alice", "sync_accounts");
        table.Rows.Add("users.bob", "sync_accounts");
        table.Rows.Add("users.alice", "cleanup_accounts");

        var filtered = ScratchBirdConnection.ApplyRestrictionValuesForMetadata(
            table,
            "procedures",
            new[] { null, "users.alice", "sync_%" });

        Assert.Single(filtered.Rows);
        Assert.Equal("users.alice", filtered.Rows[0]["schema_name"]?.ToString());
        Assert.Equal("sync_accounts", filtered.Rows[0]["procedure_name"]?.ToString());
    }

    [Fact]
    public void ApplyRestrictionValuesForMetadataFiltersRoutinesBySchemaAndName()
    {
        var table = new DataTable("Routines");
        table.Columns.Add("schema_name", typeof(string));
        table.Columns.Add("routine_name", typeof(string));
        table.Columns.Add("routine_type", typeof(string));
        table.Rows.Add("users.alice", "sync_accounts", "PROCEDURE");
        table.Rows.Add("users.alice", "sync_profiles", "FUNCTION");
        table.Rows.Add("users.bob", "sync_accounts", "PROCEDURE");

        var filtered = ScratchBirdConnection.ApplyRestrictionValuesForMetadata(
            table,
            "routines",
            new[] { null, "users.alice", "sync_acc%" });

        Assert.Single(filtered.Rows);
        Assert.Equal("users.alice", filtered.Rows[0]["schema_name"]?.ToString());
        Assert.Equal("sync_accounts", filtered.Rows[0]["routine_name"]?.ToString());
    }

    [Fact]
    public void ApplyRestrictionValuesForMetadataFiltersRoutinesUsingFunctionNameAlias()
    {
        var table = new DataTable("Routines");
        table.Columns.Add("schema_name", typeof(string));
        table.Columns.Add("function_name", typeof(string));
        table.Rows.Add("users.alice", "project_total");
        table.Rows.Add("users.alice", "project_count");

        var filtered = ScratchBirdConnection.ApplyRestrictionValuesForMetadata(
            table,
            "routines",
            new[] { null, "users.alice", "project_tot%" });

        Assert.Single(filtered.Rows);
        Assert.Equal("project_total", filtered.Rows[0]["function_name"]?.ToString());
    }

    [Fact]
    public void ApplyRestrictionValuesForMetadataFiltersTypeInfoRows()
    {
        var table = new DataTable("TypeInfo");
        table.Columns.Add("data_type_name", typeof(string));
        table.Rows.Add("int4");
        table.Rows.Add("jsonb");
        table.Rows.Add("timestamp");

        var filtered = ScratchBirdConnection.ApplyRestrictionValuesForMetadata(
            table,
            "typeinfo",
            new[] { "json%" });

        Assert.Single(filtered.Rows);
        Assert.Equal("jsonb", filtered.Rows[0]["data_type_name"]?.ToString());
    }

    [Fact]
    public void ApplyRestrictionValuesForMetadataSupportsNullRestrictionLiteral()
    {
        var table = new DataTable("Tables");
        table.Columns.Add("table_schema", typeof(string));
        table.Columns.Add("table_name", typeof(string));
        table.Columns.Add("table_type", typeof(string));
        table.Rows.Add("users.alice", DBNull.Value, "BASE TABLE");
        table.Rows.Add("users.alice", "orders", "BASE TABLE");

        var filtered = ScratchBirdConnection.ApplyRestrictionValuesForMetadata(
            table,
            "tables",
            new[] { null, "users.alice", "NULL", "BASE TABLE" });

        Assert.Single(filtered.Rows);
        Assert.True(filtered.Rows[0]["table_name"] == DBNull.Value);
    }

    [Fact]
    public void ApplyRestrictionValuesForMetadataFiltersPrimaryKeysByConstraintName()
    {
        var table = new DataTable("PrimaryKeys");
        table.Columns.Add("constraint_name", typeof(string));
        table.Columns.Add("table_id", typeof(int));
        table.Rows.Add("pk_orders", 1);
        table.Rows.Add("pk_sessions", 2);

        var filtered = ScratchBirdConnection.ApplyRestrictionValuesForMetadata(
            table,
            "primarykeys",
            new[] { null, null, null, "pk_ord%" });

        Assert.Single(filtered.Rows);
        Assert.Equal("pk_orders", filtered.Rows[0]["constraint_name"]?.ToString());
    }

    [Theory]
    [InlineData("catalog", "catalogs")]
    [InlineData("primary_keys", "primarykeys")]
    [InlineData("fk", "foreignkeys")]
    [InlineData("table_privileges", "tableprivileges")]
    [InlineData("routine", "routines")]
    [InlineData("types", "typeinfo")]
    public void NormalizeCollectionNameSupportsNewAliases(string input, string expected)
    {
        var method = typeof(ScratchBirdConnection).GetMethod(
            "NormalizeCollectionName",
            BindingFlags.NonPublic | BindingFlags.Static);
        Assert.NotNull(method);

        var normalized = (string?)method!.Invoke(null, new object?[] { input });
        Assert.Equal(expected, normalized);
    }

    [Fact]
    public void GetSchemaCatalogsReturnsSyntheticConfiguredDatabase()
    {
        using var connection = CreateOpenConnection("Host=localhost;Port=13092;Database=main;Username=sb_admin;Password=SbAdmin_Compat1!;Pooling=false");
        var catalogs = connection.GetSchema("Catalogs");
        Assert.Single(catalogs.Rows);
        Assert.Equal("main", catalogs.Rows[0]["table_catalog"]?.ToString());
    }

    private static DataTable CreateSchemasTable(params string[] schemas)
    {
        var table = new DataTable("Schemas");
        table.Columns.Add("schema_id", typeof(int));
        table.Columns.Add("schema_name", typeof(string));
        table.Columns.Add("owner_id", typeof(int));
        table.Columns.Add("default_tablespace_id", typeof(int));

        for (var i = 0; i < schemas.Length; i++)
        {
            table.Rows.Add(i + 1, schemas[i], 1, 1);
        }

        return table;
    }

    private static string[] ReadSchemaNames(DataTable table)
    {
        return table.Rows.Cast<DataRow>()
            .Select(row => row["schema_name"]?.ToString())
            .Where(name => !string.IsNullOrWhiteSpace(name))
            .Cast<string>()
            .ToArray();
    }

    private static ScratchBirdConnection CreateOpenConnection(string dsn)
    {
        var connection = new ScratchBirdConnection(dsn);
        var field = typeof(ScratchBirdConnection).GetField("_state", BindingFlags.Instance | BindingFlags.NonPublic);
        Assert.NotNull(field);
        field!.SetValue(connection, ConnectionState.Open);
        return connection;
    }
}
