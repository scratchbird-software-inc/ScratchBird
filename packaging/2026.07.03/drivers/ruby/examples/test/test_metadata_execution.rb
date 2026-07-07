# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

require "test_helper"

class TestMetadataExecution < Minitest::Test
  FakeMetadataResult = Struct.new(:rows) do
    def each_hash
      return enum_for(:each_hash) unless block_given?
      rows.each { |row| yield row }
    end
  end

  class MetadataClient < Scratchbird::Client
    attr_reader :queries

    def initialize(config, rows)
      super(config)
      @rows = rows
      @queries = []
      @connected = true
    end

    def query(sql, params = nil, options = nil)
      @queries << [sql, params, options]
      FakeMetadataResult.new(@rows.map(&:dup))
    end
  end

  class ConnectionMetadataClient
    attr_reader :calls

    def initialize(schema_rows)
      @schema_rows = schema_rows
      @calls = []
    end

    def in_transaction?
      false
    end

    def get_schema(collection_name, _options = nil, expand_schema_parents: nil)
      @calls << [collection_name, expand_schema_parents]
      @schema_rows.map(&:dup)
    end

    def get_schema_with_restrictions(collection_name, restrictions = nil, _options = nil, expand_schema_parents: nil)
      @calls << [collection_name, expand_schema_parents, restrictions]
      @schema_rows.map(&:dup)
    end
  end

  def test_query_metadata_resolves_collection_alias
    client = build_client(rows: [{ "schema_name" => "users" }])
    client.query_metadata("schema")

    assert_equal Scratchbird::Metadata::SCHEMAS_QUERY, client.queries.first[0]
  end

  def test_query_metadata_resolves_extended_collection_aliases
    client = build_client(rows: [{ "table_name" => "events" }])

    client.query_metadata("primaryKeys")
    client.query_metadata("tablePrivileges")
    client.query_metadata("ddlFields")

    assert_equal Scratchbird::Metadata::PRIMARY_KEYS_QUERY, client.queries[0][0]
    assert_equal Scratchbird::Metadata::TABLE_PRIVILEGES_QUERY, client.queries[1][0]
    assert_equal Scratchbird::Metadata::DDL_FIELDS_QUERY, client.queries[2][0]
  end

  def test_query_metadata_rejects_unknown_collection
    client = build_client(rows: [])
    err = assert_raises(Scratchbird::NotSupportedError) { client.query_metadata("missing_family") }
    assert_includes err.message, "not supported"
  end

  def test_query_metadata_with_restrictions_filters_rows
    client = build_client(
      rows: [
        { "schema_name" => "sys", "table_name" => "events" },
        { "schema_name" => "users", "table_name" => "events" },
        { "schema_name" => "users", "table_name" => "profiles" }
      ]
    )

    filtered = client.query_metadata_with_restrictions("tables", { schema: "users", table: "events" })
    assert_equal [{ "schema_name" => "users", "table_name" => "events" }], filtered.each_hash.to_a
    assert_equal Scratchbird::Metadata::TABLES_QUERY, client.queries.first[0]
  end

  def test_query_metadata_with_restrictions_supports_null_and_ignores_unknown_keys
    client = build_client(
      rows: [
        { "table_name" => "events", "owner_id" => nil },
        { "table_name" => "events", "owner_id" => 17 }
      ]
    )

    filtered = client.query_metadata_with_restrictions("tables", { owner_id: "null", missing_filter: "ignored" })
    assert_equal [{ "table_name" => "events", "owner_id" => nil }], filtered.each_hash.to_a
  end

  def test_query_metadata_with_restrictions_applies_collection_specific_aliases
    client = build_client(
      rows: [
        { "table_name" => "events", "constraint_name" => "pk_events" },
        { "table_name" => "events", "constraint_name" => "pk_logs" }
      ]
    )

    filtered = client.query_metadata_with_restrictions("primary_keys", { key_name: "pk_events", column: "ignored" })
    assert_equal [{ "table_name" => "events", "constraint_name" => "pk_events" }], filtered.each_hash.to_a
  end

  def test_query_metadata_with_restrictions_ignores_non_family_filters
    client = build_client(
      rows: [
        { "table_name" => "events", "grantee" => "alice" },
        { "table_name" => "events", "grantee" => "bob" }
      ]
    )

    filtered = client.query_metadata_with_restrictions("table_privileges", { column: "id", grantee: "alice" })
    assert_equal [{ "table_name" => "events", "grantee" => "alice" }], filtered.each_hash.to_a
  end

  def test_get_schema_expands_parent_rows_from_config
    cfg = Scratchbird::Config.new
    cfg.metadata_expand_schema_parents = true
    client = build_client(config: cfg, rows: [{ "schema_name" => "users.alice.dev", "schema_id" => 7 }])

    rows = client.get_schema("schemas")
    assert_equal ["users", "users.alice", "users.alice.dev"], rows.map { |row| row["schema_name"] }
    assert_nil rows[0]["schema_id"]
    assert_equal 7, rows[2]["schema_id"]
  end

  def test_get_schema_with_restrictions_filters_then_expands_parents
    cfg = Scratchbird::Config.new
    cfg.metadata_expand_schema_parents = true
    client = build_client(
      config: cfg,
      rows: [
        { "schema_name" => "users.alice.dev" },
        { "schema_name" => "sys.admin" }
      ]
    )

    rows = client.get_schema_with_restrictions("schemas", { schema_name: "users.alice.dev" })
    assert_equal ["users", "users.alice", "users.alice.dev"], rows.map { |row| row["schema_name"] }
  end

  def test_get_schema_tree_returns_recursive_nodes
    cfg = Scratchbird::Config.new
    cfg.metadata_expand_schema_parents = true
    client = build_client(
      config: cfg,
      rows: [
        { "schema_name" => "users.alice.dev" },
        { "schema_name" => "users.bob.dev" }
      ]
    )

    roots = client.get_schema_tree
    users = roots.find { |node| node.name == "users" }
    refute_nil users
    assert_equal %w[alice bob], users.children.map(&:name)
  end

  def test_connection_get_schema_tree_shapes_database_default_rows
    cfg = Scratchbird::Config.new
    cfg.database = "main"
    conn = Scratchbird::Connection.allocate
    conn.instance_variable_set(:@config, cfg)
    conn.instance_variable_set(
      :@client,
      ConnectionMetadataClient.new(
        [
          { "schema_name" => "users.alice.dev" },
          { "schema_name" => "users.bob.dev" }
        ]
      )
    )
    conn.instance_variable_set(:@autocommit, true)
    conn.instance_variable_set(:@closed, false)

    rows = conn.get_schema_tree(expand_schema_parents: true)
    assert_equal(
      ["main", "main.default", "main.default.users", "main.default.users.alice", "main.default.users.alice.dev", "main.default.users.bob", "main.default.users.bob.dev"],
      rows.map { |row| row["node_path"] }
    )
    assert_equal [["schemas", true, nil]], conn.client.calls
  end

  def test_connection_get_schema_with_restrictions_forwards_to_client
    cfg = Scratchbird::Config.new
    conn = Scratchbird::Connection.allocate
    conn.instance_variable_set(:@config, cfg)
    conn.instance_variable_set(:@client, ConnectionMetadataClient.new([{ "schema_name" => "users.alice.dev" }]))
    conn.instance_variable_set(:@autocommit, true)
    conn.instance_variable_set(:@closed, false)

    rows = conn.get_schema_with_restrictions("schemas", { schema: "users.alice.dev" }, nil, expand_schema_parents: true)
    assert_equal ["users.alice.dev"], rows.map { |row| row["schema_name"] }
    assert_equal [["schemas", true, { schema: "users.alice.dev" }]], conn.client.calls
  end

  private

  def build_client(rows:, config: Scratchbird::Config.new)
    MetadataClient.new(config, rows)
  end
end
