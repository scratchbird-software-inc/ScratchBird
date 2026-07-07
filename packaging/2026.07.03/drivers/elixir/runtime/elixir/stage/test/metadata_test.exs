# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBirdMetadataTest do
  use ExUnit.Case

  alias ScratchBird

  test "exposes required sys catalog metadata queries" do
    assert String.contains?(ScratchBird.schemas_query(), "FROM sys.schemas")
    assert String.contains?(ScratchBird.tables_query(), "FROM sys.tables")
    assert String.contains?(ScratchBird.columns_query(), "FROM sys.columns")
    assert String.contains?(ScratchBird.indexes_query(), "FROM sys.indexes")
    assert String.contains?(ScratchBird.index_columns_query(), "FROM sys.index_columns")
    assert String.contains?(ScratchBird.constraints_query(), "FROM sys.constraints")
    assert String.contains?(ScratchBird.procedures_query(), "FROM information_schema.routines")
    assert String.contains?(ScratchBird.functions_query(), "FROM information_schema.routines")
  end

  test "exposes richer metadata catalog families" do
    assert String.contains?(ScratchBird.routines_query(), "FROM information_schema.routines")
    assert String.contains?(ScratchBird.catalogs_query(), "FROM sys.schemas")
    assert String.contains?(ScratchBird.primary_keys_query(), "FROM sys.constraints")
    assert String.contains?(ScratchBird.foreign_keys_query(), "FROM sys.constraints")
    assert String.contains?(ScratchBird.table_privileges_query(), "FROM sys.tables")
    assert String.contains?(ScratchBird.column_privileges_query(), "FROM sys.columns")
    assert String.contains?(ScratchBird.type_info_query(), "FROM sys.columns")
  end

  test "richer metadata queries preserve expected filtering semantics" do
    assert String.contains?(ScratchBird.primary_keys_query(), "primary key")
    assert String.contains?(ScratchBird.foreign_keys_query(), "foreign key")
    assert String.contains?(ScratchBird.table_privileges_query(), "privilege_type")
    assert String.contains?(ScratchBird.column_privileges_query(), "privilege_type")
    assert String.contains?(ScratchBird.type_info_query(), "type_name")
  end
end
