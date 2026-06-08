# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

module Scratchbird
  module Metadata
    SCHEMAS_QUERY = "SELECT schema_id, schema_name, owner_id, default_tablespace_id FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name"
    TABLES_QUERY = "SELECT table_id, schema_id, table_name, table_type, owner_id FROM sys.tables WHERE is_valid = 1 ORDER BY table_name"
    COLUMNS_QUERY = "SELECT column_id, table_id, column_name, data_type_id, data_type_name, ordinal_position, is_nullable, default_value, domain_id, collation_id, charset_id, is_identity, is_generated, generation_expression FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position"
    INDEXES_QUERY = "SELECT index_id, table_id, index_name, index_type, is_unique FROM sys.indexes WHERE is_valid = 1 ORDER BY table_id, index_name"
    INDEX_COLUMNS_QUERY = "SELECT index_id, column_id, column_name, ordinal_position, is_included FROM sys.index_columns ORDER BY index_id, ordinal_position"
    CONSTRAINTS_QUERY = "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 ORDER BY table_id, constraint_name"
    PROCEDURES_QUERY = "SELECT procedure_id, schema_id, procedure_name, routine_type FROM sys.procedures WHERE is_valid = 1 ORDER BY schema_id, procedure_name"
    FUNCTIONS_QUERY = "SELECT function_id, schema_id, function_name FROM sys.functions WHERE is_valid = 1 ORDER BY schema_id, function_name"
    CATALOGS_QUERY = "SELECT DISTINCT '' AS catalog_name FROM sys.schemas ORDER BY catalog_name"
    TYPES_QUERY = "SELECT DISTINCT data_type_id AS type_id, data_type_name AS type_name FROM sys.columns WHERE is_valid = 1 ORDER BY type_name"
    PRIMARY_KEYS_QUERY = "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 AND LOWER(constraint_type) IN ('primary key', 'primary_key', 'primary') ORDER BY table_id, constraint_name"
    FOREIGN_KEYS_QUERY = "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 AND LOWER(constraint_type) IN ('foreign key', 'foreign_key', 'foreign') ORDER BY table_id, constraint_name"
    TABLE_PRIVILEGES_QUERY = "SELECT table_id, schema_id, table_name, '' AS grantor, '' AS grantee, 'SELECT' AS privilege_type, 0 AS is_grantable FROM sys.tables WHERE is_valid = 1 ORDER BY table_name"
    COLUMN_PRIVILEGES_QUERY = "SELECT c.column_id, c.table_id, c.column_name, t.table_name, '' AS grantor, '' AS grantee, 'SELECT' AS privilege_type, 0 AS is_grantable FROM sys.columns c JOIN sys.tables t ON t.table_id = c.table_id WHERE c.is_valid = 1 AND t.is_valid = 1 ORDER BY c.table_id, c.ordinal_position"
    DDL_FIELDS_QUERY = "SELECT table_id, column_id, column_name, data_type_name, default_value, generation_expression, is_nullable, is_identity, is_generated FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position"
    COLLECTION_QUERIES = {
      "schemas" => SCHEMAS_QUERY,
      "tables" => TABLES_QUERY,
      "columns" => COLUMNS_QUERY,
      "indexes" => INDEXES_QUERY,
      "index_columns" => INDEX_COLUMNS_QUERY,
      "constraints" => CONSTRAINTS_QUERY,
      "procedures" => PROCEDURES_QUERY,
      "functions" => FUNCTIONS_QUERY,
      "catalogs" => CATALOGS_QUERY,
      "types" => TYPES_QUERY,
      "primary_keys" => PRIMARY_KEYS_QUERY,
      "foreign_keys" => FOREIGN_KEYS_QUERY,
      "table_privileges" => TABLE_PRIVILEGES_QUERY,
      "column_privileges" => COLUMN_PRIVILEGES_QUERY,
      "ddl_fields" => DDL_FIELDS_QUERY
    }.freeze
    COLLECTION_ALIASES = {
      "schema" => "schemas",
      "schemas" => "schemas",
      "table" => "tables",
      "tables" => "tables",
      "column" => "columns",
      "columns" => "columns",
      "index" => "indexes",
      "indexes" => "indexes",
      "indexcolumns" => "index_columns",
      "index_columns" => "index_columns",
      "constraint" => "constraints",
      "constraints" => "constraints",
      "procedure" => "procedures",
      "procedures" => "procedures",
      "function" => "functions",
      "functions" => "functions",
      "catalog" => "catalogs",
      "catalogs" => "catalogs",
      "type" => "types",
      "types" => "types",
      "primarykey" => "primary_keys",
      "primarykeys" => "primary_keys",
      "primary_key" => "primary_keys",
      "primary_keys" => "primary_keys",
      "foreignkey" => "foreign_keys",
      "foreignkeys" => "foreign_keys",
      "foreign_key" => "foreign_keys",
      "foreign_keys" => "foreign_keys",
      "tableprivilege" => "table_privileges",
      "tableprivileges" => "table_privileges",
      "table_privilege" => "table_privileges",
      "table_privileges" => "table_privileges",
      "columnprivilege" => "column_privileges",
      "columnprivileges" => "column_privileges",
      "column_privilege" => "column_privileges",
      "column_privileges" => "column_privileges",
      "ddlfield" => "ddl_fields",
      "ddlfields" => "ddl_fields",
      "ddl_field" => "ddl_fields",
      "ddl_fields" => "ddl_fields"
    }.freeze
    SCHEMA_FIELD_CANDIDATES = %w[
      schema_name
      table_schem
      table_schema
      schema
      SCHEMA_NAME
      TABLE_SCHEM
      TABLE_SCHEMA
      SCHEMA
    ].freeze
    RESTRICTION_KEY_ALIASES = {
      "catalog" => %w[catalog_name table_catalog table_cat catalog],
      "schema" => %w[schema_name table_schema table_schem schema],
      "table" => %w[table_name table relname],
      "column" => %w[column_name column],
      "index" => %w[index_name index],
      "constraint" => %w[constraint_name constraint],
      "procedure" => %w[procedure_name routine_name],
      "function" => %w[function_name routine_name],
      "type" => %w[type_name data_type_name data_type udt_name],
      "grantor" => %w[grantor],
      "grantee" => %w[grantee],
      "privilege" => %w[privilege_type privilege],
      "ownerid" => %w[owner_id ownerid owner],
      "pkname" => %w[pk_name primary_key_name constraint_name],
      "fkname" => %w[fk_name foreign_key_name constraint_name],
      "keyname" => %w[key_name constraint_name]
    }.freeze
    COLLECTION_RESTRICTION_KEYS = {
      "schemas" => %w[catalog schema],
      "tables" => %w[catalog schema table type ownerid],
      "columns" => %w[catalog schema table column type],
      "indexes" => %w[catalog schema table index],
      "index_columns" => %w[catalog schema table index column],
      "constraints" => %w[catalog schema table constraint],
      "procedures" => %w[catalog schema procedure],
      "functions" => %w[catalog schema function],
      "catalogs" => %w[catalog],
      "types" => %w[catalog schema type],
      "primary_keys" => %w[catalog schema table key_name pk_name constraint],
      "foreign_keys" => %w[catalog schema table key_name fk_name constraint],
      "table_privileges" => %w[catalog schema table grantee grantor privilege],
      "column_privileges" => %w[catalog schema table column grantee grantor privilege],
      "ddl_fields" => %w[catalog schema table column type]
    }.freeze

    class SchemaTreeNode
      attr_reader :name, :full_path, :children
      attr_accessor :terminal

      def initialize(name, full_path)
        @name = name
        @full_path = full_path
        @terminal = false
        @children = []
      end
    end

    def self.schemas_query
      SCHEMAS_QUERY
    end

    def self.tables_query
      TABLES_QUERY
    end

    def self.columns_query
      COLUMNS_QUERY
    end

    def self.indexes_query
      INDEXES_QUERY
    end

    def self.index_columns_query
      INDEX_COLUMNS_QUERY
    end

    def self.constraints_query
      CONSTRAINTS_QUERY
    end

    def self.procedures_query
      PROCEDURES_QUERY
    end

    def self.functions_query
      FUNCTIONS_QUERY
    end

    def self.normalize_collection_name(collection_name = "tables")
      raw = collection_name.to_s.strip.downcase
      raw = "tables" if raw.empty?
      collapsed = raw.gsub(/[^a-z0-9]/, "")
      normalized = COLLECTION_ALIASES[raw] || COLLECTION_ALIASES[collapsed]
      return normalized if normalized

      raise ArgumentError, "Metadata collection '#{collection_name}' is not supported"
    end

    def self.resolve_collection_query(collection_name = "tables")
      COLLECTION_QUERIES.fetch(normalize_collection_name(collection_name))
    end

    def self.normalize_restrictions(restrictions)
      return {} if restrictions.nil?
      return {} if restrictions.respond_to?(:empty?) && restrictions.empty?
      unless restrictions.respond_to?(:each_pair)
        raise ArgumentError, "metadata restrictions must be provided as a hash"
      end

      out = {}
      restrictions.each_pair do |key, value|
        normalized = normalize_identifier(key)
        next if normalized.empty?

        out[normalized] = value
      end
      out
    end

    def self.filter_rows_by_restrictions(rows, restrictions, collection_name: nil)
      normalized_restrictions = normalize_restrictions(restrictions)
      return rows if normalized_restrictions.empty?

      bindings = build_restriction_bindings(rows, normalized_restrictions, collection_name)
      return rows if bindings.empty?

      rows.select { |row| row_matches_restrictions?(row, bindings) }
    end

    def self.schema_paths_for_navigation(rows_or_names, expand_schema_parents: false)
      base_paths = unique_schema_paths(rows_or_names)
      return base_paths unless expand_schema_parents

      expand_schema_parent_paths(base_paths)
    end

    def self.expand_schema_parent_paths(rows_or_names)
      out = []
      seen = {}

      each_schema_path(rows_or_names) do |schema_path|
        current = +""
        split_schema_path(schema_path).each do |segment|
          current = current.empty? ? segment : "#{current}.#{segment}"
          next if seen[current]

          seen[current] = true
          out << current
        end
      end

      out
    end

    def self.build_schema_tree(schema_paths)
      nodes_by_path = {}
      roots = []

      each_schema_path(schema_paths) do |schema_path|
        parent = nil
        path_parts = []

        split_schema_path(schema_path).each do |segment|
          path_parts << segment
          full_path = path_parts.join(".")

          node = nodes_by_path[full_path]
          unless node
            node = SchemaTreeNode.new(segment, full_path)
            nodes_by_path[full_path] = node
            if parent
              parent.children << node
            else
              roots << node
            end
          end

          parent = node
        end

        parent.terminal = true if parent
      end

      roots
    end

    def self.expand_schema_metadata_rows(rows)
      out = []
      seen = {}

      rows.each do |row|
        schema_path = read_schema_path(row)
        unless schema_path
          out << row
          next
        end

        current = +""
        segments = split_schema_path(schema_path)
        segments.each_with_index do |segment, idx|
          current = current.empty? ? segment : "#{current}.#{segment}"
          next if seen[current]

          seen[current] = true
          if idx == segments.length - 1
            out << row
          else
            out << synthetic_schema_row(row, current)
          end
        end
      end

      out
    end

    def self.build_database_default_metadata_rows(rows_or_names, database:, expand_schema_parents: false, default_branch: "default")
      database_name = database.to_s.strip
      raise ArgumentError, "database is required" if database_name.empty?

      default_name = default_branch.to_s.strip
      raise ArgumentError, "default_branch is required" if default_name.empty?

      schema_paths = schema_paths_for_navigation(rows_or_names, expand_schema_parents: expand_schema_parents)
      roots = build_schema_tree(schema_paths)
      rows = []

      database_path = database_name
      default_path = "#{database_path}.#{default_name}"

      rows << {
        "node_type" => "database",
        "node_name" => database_name,
        "node_path" => database_path,
        "parent_path" => nil,
        "schema_path" => nil,
        "terminal" => false
      }

      rows << {
        "node_type" => "schema",
        "node_name" => default_name,
        "node_path" => default_path,
        "parent_path" => database_path,
        "schema_path" => nil,
        "terminal" => false
      }

      append_tree_metadata_rows(rows, roots, default_path)
      rows
    end

    def self.append_tree_metadata_rows(out_rows, nodes, parent_node_path)
      nodes.each do |node|
        node_path = "#{parent_node_path}.#{node.name}"
        out_rows << {
          "node_type" => "schema",
          "node_name" => node.name,
          "node_path" => node_path,
          "parent_path" => parent_node_path,
          "schema_path" => node.full_path,
          "terminal" => node.terminal
        }
        append_tree_metadata_rows(out_rows, node.children, node_path)
      end
    end
    private_class_method :append_tree_metadata_rows

    def self.unique_schema_paths(rows_or_names)
      out = []
      seen = {}

      each_schema_path(rows_or_names) do |schema_path|
        next if seen[schema_path]

        seen[schema_path] = true
        out << schema_path
      end

      out
    end
    private_class_method :unique_schema_paths

    def self.each_schema_path(rows_or_names)
      return enum_for(:each_schema_path, rows_or_names) unless block_given?
      return if rows_or_names.nil?

      enumerated =
        if rows_or_names.is_a?(String) || rows_or_names.is_a?(Hash)
          [rows_or_names]
        elsif rows_or_names.respond_to?(:each)
          rows_or_names
        else
          [rows_or_names]
        end

      enumerated.each do |row_or_name|
        schema_path = read_schema_path(row_or_name)
        next unless schema_path

        yield schema_path
      end
    end
    private_class_method :each_schema_path

    def self.read_schema_path(row_or_name)
      return normalize_schema_path(row_or_name) if row_or_name.is_a?(String)

      return normalize_schema_path(row_or_name.to_s) if row_or_name.is_a?(Symbol)

      return read_schema_path_from_hash(row_or_name) if row_or_name.is_a?(Hash)

      nil
    end
    private_class_method :read_schema_path

    def self.read_schema_path_from_hash(row)
      SCHEMA_FIELD_CANDIDATES.each do |candidate|
        value = nil
        value = row[candidate] if row.key?(candidate)
        symbol = candidate.to_sym
        value = row[symbol] if value.nil? && row.key?(symbol)
        next unless value.is_a?(String)

        normalized = normalize_schema_path(value)
        return normalized if normalized
      end

      nil
    end
    private_class_method :read_schema_path_from_hash

    def self.normalize_schema_path(value)
      normalized = split_schema_path(value).join(".")
      return nil if normalized.empty?

      normalized
    end
    private_class_method :normalize_schema_path

    def self.split_schema_path(value)
      value.to_s.split(".").map(&:strip).reject(&:empty?)
    end
    private_class_method :split_schema_path

    def self.synthetic_schema_row(sample_row, schema_path)
      synthetic = {}
      sample_row.each_key do |key|
        synthetic[key] = nil
      end
      assign_schema_path(synthetic, schema_path)
      synthetic
    end
    private_class_method :synthetic_schema_row

    def self.assign_schema_path(row, schema_path)
      assigned = false
      SCHEMA_FIELD_CANDIDATES.each do |candidate|
        if row.key?(candidate)
          row[candidate] = schema_path
          assigned = true
        end
        symbol = candidate.to_sym
        if row.key?(symbol)
          row[symbol] = schema_path
          assigned = true
        end
      end
      return if assigned

      target_key = row.keys.any? { |key| key.is_a?(Symbol) } ? :schema_name : "schema_name"
      row[target_key] = schema_path
    end
    private_class_method :assign_schema_path

    def self.build_restriction_bindings(rows, normalized_restrictions, collection_name)
      aliases_allowed =
        if collection_name
          normalized_collection = normalize_collection_name(collection_name)
          Array(COLLECTION_RESTRICTION_KEYS[normalized_collection]).flat_map do |key|
            metadata_restriction_key_aliases(key)
          end.map { |alias_key| normalize_identifier(alias_key) }.reject(&:empty?).uniq
        else
          []
        end

      bindings = normalized_restrictions.map do |key, value|
        normalized_key = normalize_identifier(key)
        if !aliases_allowed.empty? && !aliases_allowed.include?(normalized_key)
          next
        end
        aliases = metadata_restriction_key_aliases(key)
                  .map { |alias_key| normalize_identifier(alias_key) }
                  .reject(&:empty?)
                  .uniq
        aliases &= aliases_allowed unless aliases_allowed.empty?
        aliases |= [normalized_key] if aliases_allowed.empty? || aliases_allowed.include?(normalized_key)
        next if aliases.empty?

        expected_text = normalize_match_text(value)
        expect_null = expected_text == "null"
        {
          aliases: aliases,
          expect_null: expect_null,
          expected_text: expect_null ? nil : expected_text
        }
      end.compact

      bindings.select do |binding|
        rows_have_binding_aliases?(rows: rows, binding: binding)
      end
    end
    private_class_method :build_restriction_bindings

    def self.row_matches_restrictions?(row, bindings)
      bindings.all? do |binding|
        values = metadata_values_for_aliases(row, binding[:aliases])
        next false if values.empty?

        values.any? do |value|
          if binding[:expect_null]
            value.nil?
          else
            !value.nil? && normalize_match_text(value) == binding[:expected_text]
          end
        end
      end
    end
    private_class_method :row_matches_restrictions?

    def self.metadata_restriction_key_aliases(key)
      canonical = normalize_identifier(key)
      aliases = Array(RESTRICTION_KEY_ALIASES[canonical])
      aliases << canonical unless canonical.empty?
      aliases
    end
    private_class_method :metadata_restriction_key_aliases

    def self.metadata_values_for_aliases(row, aliases)
      return [] unless row.is_a?(Hash)

      values = []
      aliases.each do |alias_key|
        present, value = metadata_row_value(row, alias_key)
        values << value if present
      end
      values
    end
    private_class_method :metadata_values_for_aliases

    def self.metadata_row_value(row, alias_key)
      return [true, row[alias_key]] if row.key?(alias_key)

      symbol = alias_key.to_sym
      return [true, row[symbol]] if row.key?(symbol)

      target = normalize_identifier(alias_key)
      row.each do |candidate, value|
        return [true, value] if normalize_identifier(candidate) == target
      end
      [false, nil]
    end
    private_class_method :metadata_row_value

    def self.rows_have_binding_aliases?(rows:, binding:)
      return false unless rows.respond_to?(:any?)

      rows.any? do |row|
        binding[:aliases].any? { |alias_key| metadata_row_value(row, alias_key).first }
      end
    end
    private_class_method :rows_have_binding_aliases?

    def self.normalize_identifier(value)
      value.to_s.strip.downcase.gsub(/[^a-z0-9]/, "")
    end
    private_class_method :normalize_identifier

    def self.normalize_match_text(value)
      value.to_s.strip.downcase
    end
    private_class_method :normalize_match_text
  end
end
