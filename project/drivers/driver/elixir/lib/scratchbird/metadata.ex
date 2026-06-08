# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBird.Metadata do
  @moduledoc false

  def schemas_query do
    "SELECT schema_id, schema_name, owner_id, default_tablespace_id FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name"
  end

  def tables_query do
    "SELECT table_id, schema_id, table_name, table_type, owner_id FROM sys.tables WHERE is_valid = 1 ORDER BY table_name"
  end

  def columns_query do
    "SELECT column_id, table_id, column_name, data_type_id, data_type_name, ordinal_position, is_nullable, default_value, domain_id, collation_id, charset_id, is_identity, is_generated, generation_expression FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position"
  end

  def indexes_query do
    "SELECT index_id, table_id, index_name, index_type, is_unique FROM sys.indexes WHERE is_valid = 1 ORDER BY table_id, index_name"
  end

  def index_columns_query do
    "SELECT index_id, column_id, column_name, ordinal_position, is_included FROM sys.index_columns ORDER BY index_id, ordinal_position"
  end

  def constraints_query do
    "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 ORDER BY table_id, constraint_name"
  end

  def procedures_query do
    "SELECT routine_schema AS schema_id, routine_schema AS schema_name, routine_schema AS table_schema, routine_schema AS table_schem, routine_name AS procedure_name, routine_name AS routine_name, routine_name AS specific_name, routine_type FROM information_schema.routines WHERE lower(routine_type) = 'procedure' ORDER BY schema_name, procedure_name"
  end

  def functions_query do
    "SELECT routine_schema AS schema_id, routine_schema AS schema_name, routine_schema AS table_schema, routine_schema AS table_schem, routine_name AS function_name, routine_name AS routine_name, routine_name AS specific_name, routine_type FROM information_schema.routines WHERE lower(routine_type) = 'function' ORDER BY schema_name, function_name"
  end

  def routines_query do
    "SELECT routine_schema AS schema_id, routine_schema AS schema_name, routine_schema AS table_schema, routine_schema AS table_schem, routine_name AS routine_name, routine_name AS specific_name, routine_type FROM information_schema.routines ORDER BY schema_name, routine_name"
  end

  def catalogs_query do
    "SELECT schema_id AS catalog_id, schema_name AS catalog_name FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name"
  end

  def primary_keys_query do
    "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 AND lower(constraint_type) IN ('primary key', 'primary') ORDER BY table_id, constraint_name"
  end

  def foreign_keys_query do
    "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 AND lower(constraint_type) IN ('foreign key', 'foreign') ORDER BY table_id, constraint_name"
  end

  def table_privileges_query do
    "SELECT table_id, table_name, owner_id AS grantor_id, owner_id AS grantee_id, 'ALL' AS privilege_type FROM sys.tables WHERE is_valid = 1 ORDER BY table_id, table_name"
  end

  def column_privileges_query do
    "SELECT table_id, column_id, column_name, 'ALL' AS privilege_type FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position"
  end

  def type_info_query do
    "SELECT DISTINCT data_type_id, data_type_name, data_type_name AS type_name FROM sys.columns WHERE is_valid = 1 ORDER BY data_type_name"
  end
end
