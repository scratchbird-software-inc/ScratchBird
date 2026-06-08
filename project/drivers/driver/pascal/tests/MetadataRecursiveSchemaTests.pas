// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

program MetadataRecursiveSchemaTests;

{$mode delphi}
{$APPTYPE CONSOLE}

uses
  {$IFDEF UNIX}
  cthreads,
  {$ENDIF}
  SysUtils, Variants, ScratchBird.Metadata, ScratchBird.Errors, ScratchBird.Client;

procedure Fail(const MessageText: string);
begin
  raise Exception.Create(MessageText);
end;

procedure AssertTrue(Value: Boolean; const MessageText: string);
begin
  if not Value then
    Fail(MessageText);
end;

procedure AssertEqualInt(Expected, Actual: Integer; const MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected=' + IntToStr(Expected) + ' actual=' + IntToStr(Actual));
end;

procedure AssertEqualString(const Expected, Actual, MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected="' + Expected + '" actual="' + Actual + '"');
end;

procedure AssertContains(const Needle, Haystack, MessageText: string);
begin
  if Pos(Needle, Haystack) = 0 then
    Fail(MessageText + ': expected "' + Needle + '" in "' + Haystack + '"');
end;

procedure AssertVariantInt(Expected: Integer; const Value: Variant; const MessageText: string);
begin
  if VarIsNull(Value) or VarIsEmpty(Value) then
    Fail(MessageText + ': expected integer but value is null/empty');
  if Integer(Value) <> Expected then
    Fail(MessageText + ': expected=' + IntToStr(Expected) + ' actual=' + IntToStr(Integer(Value)));
end;

function MetadataField(const Name: string; const Value: Variant): TMetadataField;
begin
  Result.Name := Name;
  Result.Value := Value;
end;

function MetadataRow(const Fields: array of TMetadataField): TMetadataRow;
var
  I: Integer;
begin
  Result := nil;
  SetLength(Result, Length(Fields));
  for I := 0 to High(Fields) do
    Result[I] := Fields[I];
end;

function SchemaRow(const SchemaName: string): TMetadataRow;
begin
  Result := MetadataRow([MetadataField('schema_name', SchemaName)]);
end;

function StringArray(const Values: array of string): TArray<string>;
var
  I: Integer;
begin
  Result := nil;
  SetLength(Result, Length(Values));
  for I := 0 to High(Values) do
    Result[I] := Values[I];
end;

function CollectSchemaValues(const Rows: TMetadataRows; const Key: string): TArray<string>;
var
  Value: Variant;
  I, Count: Integer;
begin
  Result := nil;
  SetLength(Result, 0);
  for I := 0 to High(Rows) do
  begin
    if not MetadataRowTryGetValue(Rows[I], Key, Value) then
      Continue;
    if VarIsNull(Value) or VarIsEmpty(Value) then
      Continue;
    Count := Length(Result);
    SetLength(Result, Count + 1);
    Result[Count] := VarToStr(Value);
  end;
end;

procedure AssertEqualStringArray(const Expected, Actual: TArray<string>; const MessageText: string);
var
  I: Integer;
begin
  if Length(Expected) <> Length(Actual) then
    Fail(MessageText + ': expected count=' + IntToStr(Length(Expected)) + ' actual count=' + IntToStr(Length(Actual)));
  for I := 0 to High(Expected) do
    if Expected[I] <> Actual[I] then
      Fail(MessageText + ': mismatch at index ' + IntToStr(I) + ' expected="' + Expected[I] + '" actual="' + Actual[I] + '"');
end;

function CountChildrenByName(Node: TMetadataSchemaTreeNode; const Name: string): Integer;
var
  I: Integer;
begin
  Result := 0;
  for I := 0 to Node.ChildCount - 1 do
  begin
    if Node.Children[I].Name = Name then
      Inc(Result);
  end;
end;

procedure TestExpandMetadataRowsSupportsDatabaseDefaultBranchStyleRows;
var
  Rows: TMetadataRows;
  Expanded: TMetadataRows;
  SchemaId: Variant;
begin
  SetLength(Rows, 2);
  Rows[0] := MetadataRow([
    MetadataField('schema_id', 11),
    MetadataField('TABLE_SCHEM', 'database.default.users'),
    MetadataField('TABLE_CATALOG', 'database')
  ]);
  Rows[1] := MetadataRow([
    MetadataField('schema_id', 12),
    MetadataField('TABLE_SCHEM', 'database.default.audit'),
    MetadataField('TABLE_CATALOG', 'database')
  ]);

  Expanded := ExpandSchemaMetadataRows(Rows);

  AssertEqualStringArray(
    StringArray(['database', 'database.default', 'database.default.users', 'database.default.audit']),
    CollectSchemaValues(Expanded, 'TABLE_SCHEM'),
    'database/default branch-style schema expansion');

  AssertTrue(MetadataRowTryGetValue(Expanded[0], 'schema_id', SchemaId), 'expanded row 0 should include schema_id');
  AssertTrue(VarIsNull(SchemaId), 'expanded row 0 schema_id should be null');
  AssertTrue(MetadataRowTryGetValue(Expanded[1], 'schema_id', SchemaId), 'expanded row 1 should include schema_id');
  AssertTrue(VarIsNull(SchemaId), 'expanded row 1 schema_id should be null');
  AssertTrue(MetadataRowTryGetValue(Expanded[2], 'schema_id', SchemaId), 'expanded row 2 should include schema_id');
  AssertVariantInt(11, SchemaId, 'expanded row 2 schema_id');
  AssertTrue(MetadataRowTryGetValue(Expanded[3], 'schema_id', SchemaId), 'expanded row 3 should include schema_id');
  AssertVariantInt(12, SchemaId, 'expanded row 3 schema_id');
end;

procedure TestListMetadataSchemaPathsExpandsDottedParents;
var
  Rows: TMetadataRows;
  ExpandedPaths: TArray<string>;
begin
  SetLength(Rows, 6);
  Rows[0] := SchemaRow('users.alice.dev');
  Rows[1] := SchemaRow('sys');
  Rows[2] := SchemaRow('users.bob.dev');
  Rows[3] := SchemaRow('users.bob.dev');
  Rows[4] := SchemaRow('users..bob.dev');
  Rows[5] := SchemaRow('');

  ExpandedPaths := ListMetadataSchemaPaths(Rows, True);
  AssertEqualStringArray(
    StringArray(['users', 'users.alice', 'users.alice.dev', 'sys', 'users.bob', 'users.bob.dev']),
    ExpandedPaths,
    'dotted parent expansion order and uniqueness');
end;

procedure TestBuildMetadataSchemaTreeEnforcesPerParentUniqueness;
var
  Rows: TMetadataRows;
  Tree: TMetadataSchemaTree;
  BobNode: TMetadataSchemaTreeNode;
begin
  SetLength(Rows, 3);
  Rows[0] := SchemaRow('users.bob.dev');
  Rows[1] := SchemaRow('users.bob.dev');
  Rows[2] := SchemaRow('users.bob.prod');

  Tree := BuildMetadataSchemaTree(Rows, False);
  try
    BobNode := Tree.FindNodeByPath('users.bob');
    AssertTrue(BobNode <> nil, 'users.bob node should exist');
    AssertEqualInt(2, BobNode.ChildCount, 'users.bob child count');
    AssertEqualString('users.bob.dev', BobNode.Children[0].Path, 'first child path');
    AssertEqualString('users.bob.prod', BobNode.Children[1].Path, 'second child path');
    AssertEqualInt(1, CountChildrenByName(BobNode, 'dev'), 'unique child names per parent');
  finally
    Tree.Free;
  end;
end;

procedure TestBuildMetadataSchemaTreeAllowsSameLeafUnderDifferentParents;
var
  Rows: TMetadataRows;
  Tree: TMetadataSchemaTree;
  AliceDev, BobDev: TMetadataSchemaTreeNode;
begin
  SetLength(Rows, 2);
  Rows[0] := SchemaRow('users.alice.dev');
  Rows[1] := SchemaRow('users.bob.dev');

  Tree := BuildMetadataSchemaTree(Rows, True, 'demo');
  try
    AssertEqualString('demo', Tree.Database, 'tree database label');
    AliceDev := Tree.FindNodeByPath('users.alice.dev');
    BobDev := Tree.FindNodeByPath('users.bob.dev');
    AssertTrue(AliceDev <> nil, 'users.alice.dev should exist');
    AssertTrue(BobDev <> nil, 'users.bob.dev should exist');
    AssertEqualString('dev', AliceDev.Name, 'alice leaf name');
    AssertEqualString('dev', BobDev.Name, 'bob leaf name');
    AssertTrue(AliceDev.Path <> BobDev.Path, 'leaf paths should differ');
    AssertTrue(AliceDev <> BobDev, 'leaf nodes should be distinct instances');
  finally
    Tree.Free;
  end;
end;

procedure TestMetadataCollectionResolution;
var
  SqlText: string;
begin
  AssertEqualString('schemas', NormalizeMetadataCollectionName('schema'), 'schema alias normalization');
  AssertEqualString('index_columns', NormalizeMetadataCollectionName('indexColumns'), 'index columns alias normalization');
  AssertEqualString('catalogs', NormalizeMetadataCollectionName('catalog'), 'catalog alias normalization');
  AssertEqualString('primary_keys', NormalizeMetadataCollectionName('pk'), 'primary keys alias normalization');
  AssertEqualString('foreign_keys', NormalizeMetadataCollectionName('fk'), 'foreign keys alias normalization');
  AssertEqualString('type_info', NormalizeMetadataCollectionName('typeinfo'), 'type info alias normalization');
  AssertEqualString('routines', NormalizeMetadataCollectionName('routine'), 'routine alias normalization');
  SqlText := ResolveMetadataCollectionQuery('constraints');
  AssertContains('FROM sys.constraints', SqlText, 'constraints query resolution');
  SqlText := ResolveMetadataCollectionQuery('catalogs');
  AssertContains('AS catalog_name', SqlText, 'catalogs query resolution');
  SqlText := ResolveMetadataCollectionQuery('routines');
  AssertContains('UNION ALL', SqlText, 'routines query resolution');
  AssertContains('specific_name', SqlText, 'routines query includes specific_name alias');
  SqlText := ResolveMetadataCollectionQuery('primary_keys');
  AssertContains('lower(constraint_type)', SqlText, 'primary keys query resolution');
  SqlText := ResolveMetadataCollectionQuery('foreign_keys');
  AssertContains('foreign key', SqlText, 'foreign keys query resolution');
  SqlText := ResolveMetadataCollectionQuery('index_columns');
  AssertContains('JOIN sys.indexes', SqlText, 'index columns query resolution');
  AssertContains('index_name', SqlText, 'index columns includes index_name');
  AssertContains('AS table_schema', SqlText, 'index columns includes table_schema alias');
  SqlText := ResolveMetadataCollectionQuery('table_privileges');
  AssertContains('privilege_type', SqlText, 'table privileges query resolution');
  SqlText := ResolveMetadataCollectionQuery('column_privileges');
  AssertContains('FROM sys.columns', SqlText, 'column privileges query resolution');
  SqlText := ResolveMetadataCollectionQuery('type_info');
  AssertContains('SELECT DISTINCT', SqlText, 'type info query resolution');
  AssertContains('AS type_name', SqlText, 'type info query includes type_name alias');

  try
    ResolveMetadataCollectionQuery('unsupported_metadata_family');
    Fail('unsupported metadata collection should raise not supported');
  except
    on E: EScratchbirdNotSupported do
    begin
      AssertEqualString('0A000', E.SQLState, 'unsupported metadata SQLSTATE');
      AssertContains('not supported', E.Message, 'unsupported metadata message');
    end;
  end;
end;

procedure TestFilterMetadataRowsByRestrictionsSupportsAliasesWildcardAndNull;
var
  Rows: TMetadataRows;
  Restrictions: TMetadataRow;
  Filtered: TMetadataRows;
begin
  SetLength(Rows, 3);
  Rows[0] := MetadataRow([
    MetadataField('table_schema', 'users'),
    MetadataField('table_name', 'accounts'),
    MetadataField('column_name', 'id'),
    MetadataField('data_type_name', 'INTEGER')
  ]);
  Rows[1] := MetadataRow([
    MetadataField('table_schema', 'users'),
    MetadataField('table_name', 'accounts'),
    MetadataField('column_name', 'email'),
    MetadataField('data_type_name', Null)
  ]);
  Rows[2] := MetadataRow([
    MetadataField('table_schema', 'sys'),
    MetadataField('table_name', 'catalog_tables'),
    MetadataField('column_name', 'table_name'),
    MetadataField('data_type_name', 'VARCHAR')
  ]);

  Restrictions := MetadataRow([
    MetadataField('schema', 'users'),
    MetadataField('table', 'acc%')
  ]);
  Filtered := FilterMetadataRowsByRestrictions(Rows, Restrictions, 'columns');
  AssertEqualStringArray(
    StringArray(['id', 'email']),
    CollectSchemaValues(Filtered, 'column_name'),
    'schema/table restriction filtering');

  Restrictions := MetadataRow([
    MetadataField('schema', 'users'),
    MetadataField('column', 'e%')
  ]);
  Filtered := FilterMetadataRowsByRestrictions(Rows, Restrictions, 'columns');
  AssertEqualStringArray(
    StringArray(['email']),
    CollectSchemaValues(Filtered, 'column_name'),
    'wildcard column restriction filtering');

  Restrictions := MetadataRow([MetadataField('type', 'null')]);
  Filtered := FilterMetadataRowsByRestrictions(Rows, Restrictions, 'columns');
  AssertEqualStringArray(
    StringArray(['email']),
    CollectSchemaValues(Filtered, 'column_name'),
    'null-literal restriction filtering');

  Restrictions := MetadataRow([
    MetadataField('schema', 'users'),
    MetadataField('unsupported_key', 'ignored')
  ]);
  Filtered := FilterMetadataRowsByRestrictions(Rows, Restrictions, 'columns');
  AssertEqualStringArray(
    StringArray(['id', 'email']),
    CollectSchemaValues(Filtered, 'column_name'),
    'unsupported restriction keys are ignored');
end;

procedure TestFilterMetadataRowsByRestrictionsSupportsRoutineAliases;
var
  Rows: TMetadataRows;
  Restrictions: TMetadataRow;
  Filtered: TMetadataRows;
begin
  SetLength(Rows, 3);
  Rows[0] := MetadataRow([
    MetadataField('routine_name', 'refresh_cache'),
    MetadataField('routine_type', 'PROCEDURE')
  ]);
  Rows[1] := MetadataRow([
    MetadataField('routine_name', 'to_json_text'),
    MetadataField('routine_type', 'FUNCTION')
  ]);
  Rows[2] := MetadataRow([
    MetadataField('routine_name', 'cleanup_expired_tokens'),
    MetadataField('routine_type', 'PROCEDURE')
  ]);

  Restrictions := MetadataRow([MetadataField('procedure', 'refresh_cache')]);
  Filtered := FilterMetadataRowsByRestrictions(Rows, Restrictions, 'routines');
  AssertEqualStringArray(
    StringArray(['refresh_cache']),
    CollectSchemaValues(Filtered, 'routine_name'),
    'routines procedure restriction filtering');

  Restrictions := MetadataRow([MetadataField('function', 'to_json%')]);
  Filtered := FilterMetadataRowsByRestrictions(Rows, Restrictions, 'routines');
  AssertEqualStringArray(
    StringArray(['to_json_text']),
    CollectSchemaValues(Filtered, 'routine_name'),
    'routines function wildcard restriction filtering');
end;

procedure TestClientMetadataApiGuards;
var
  Client: TScratchBirdClient;
begin
  Client := TScratchBirdClient.Create;
  try
    try
      Client.QueryMetadata('unsupported_metadata_family');
      Fail('unsupported metadata collection should fail before connect');
    except
      on E: EScratchbirdNotSupported do
      begin
        AssertEqualString('0A000', E.SQLState, 'client metadata unsupported SQLSTATE');
        AssertContains('not supported', E.Message, 'client metadata unsupported message');
      end;
    end;

    try
      Client.GetSchema('tables');
      Fail('supported metadata collection should require connected client');
    except
      on E: EScratchbirdConnectionError do
      begin
        AssertEqualString('08003', E.SQLState, 'client metadata disconnected SQLSTATE');
        AssertContains('not connected', E.Message, 'client metadata disconnected message');
      end;
    end;
  finally
    Client.Free;
  end;
end;

procedure TestClientMetadataRowsApiGuards;
var
  Client: TScratchBirdClient;
  Restrictions: TMetadataRow;
begin
  Client := TScratchBirdClient.Create;
  try
    Restrictions := MetadataRow([MetadataField('schema', 'users')]);

    try
      Client.QueryMetadataRows('unsupported_metadata_family', Restrictions);
      Fail('unsupported metadata collection should fail for QueryMetadataRows');
    except
      on E: EScratchbirdNotSupported do
        AssertEqualString('0A000', E.SQLState, 'QueryMetadataRows unsupported SQLSTATE');
    end;

    try
      Client.QueryMetadataRows('tables', Restrictions);
      Fail('supported metadata QueryMetadataRows should require connected client');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'QueryMetadataRows disconnected SQLSTATE');
    end;
  finally
    Client.Free;
  end;
end;

procedure TestTypedMetadataApiGuards;
var
  Client: TScratchBirdClient;
begin
  Client := TScratchBirdClient.Create;
  try
    try
      Client.GetCatalogs;
      Fail('GetCatalogs should require connected client');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'GetCatalogs disconnected SQLSTATE');
    end;

    try
      Client.GetRoutines;
      Fail('GetRoutines should require connected client');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'GetRoutines disconnected SQLSTATE');
    end;

    try
      Client.GetIndexColumns;
      Fail('GetIndexColumns should require connected client');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'GetIndexColumns disconnected SQLSTATE');
    end;

    try
      Client.GetTypeInfo;
      Fail('GetTypeInfo should require connected client');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'GetTypeInfo disconnected SQLSTATE');
    end;
  finally
    Client.Free;
  end;
end;

begin
  try
    TestExpandMetadataRowsSupportsDatabaseDefaultBranchStyleRows;
    TestListMetadataSchemaPathsExpandsDottedParents;
    TestBuildMetadataSchemaTreeEnforcesPerParentUniqueness;
    TestBuildMetadataSchemaTreeAllowsSameLeafUnderDifferentParents;
    TestMetadataCollectionResolution;
    TestFilterMetadataRowsByRestrictionsSupportsAliasesWildcardAndNull;
    TestFilterMetadataRowsByRestrictionsSupportsRoutineAliases;
    TestClientMetadataApiGuards;
    TestClientMetadataRowsApiGuards;
    TestTypedMetadataApiGuards;
    Writeln('MetadataRecursiveSchemaTests: OK');
  except
    on E: Exception do
    begin
      Writeln('MetadataRecursiveSchemaTests: FAILED - ' + E.Message);
      Halt(1);
    end;
  end;
end.
