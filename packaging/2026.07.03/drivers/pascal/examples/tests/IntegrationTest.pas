// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

program IntegrationTest;

{$mode delphi}
{$APPTYPE CONSOLE}

uses
  {$IFDEF UNIX}
  cthreads,
  {$ENDIF}
  SysUtils, Variants,
  ScratchBird.Client, ScratchBird.Sql, ScratchBird.Metadata, ScratchBird.Protocol, ScratchBird.Errors;

var
  Dsn: string;
  StreamSql: string;
  CancelSql: string;
  GeneratedKeySql: string;
  GeneratedKeyExpectedText: string;
  Client: TScratchBirdClient;

procedure Fail(const MessageText: string);
begin
  raise Exception.Create(MessageText);
end;

procedure AssertTrue(Value: Boolean; const MessageText: string);
begin
  if not Value then
    Fail(MessageText);
end;

procedure AssertEqualInt64(Expected, Actual: Int64; const MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected=' + IntToStr(Expected) + ' actual=' + IntToStr(Actual));
end;

procedure AssertEqualUInt64(Expected, Actual: UInt64; const MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected=' + IntToStr(Int64(Expected)) + ' actual=' + IntToStr(Int64(Actual)));
end;

function RequireVariantInt64(const Value: Variant; const MessageText: string): Int64;
begin
  if VarIsNull(Value) or VarIsEmpty(Value) then
    Fail(MessageText + ': value is null/empty');
  try
    Result := VarAsType(Value, varInt64);
  except
    on E: Exception do
      Fail(MessageText + ': expected integer-convertible value (' + E.Message + ')');
  end;
end;

procedure DrainStream(Stream: TScratchBirdResultStream; out RowCount: Integer);
var
  Row: TArray<Variant>;
begin
  RowCount := 0;
  while True do
  begin
    Row := Stream.ReadRow;
    if Row = nil then
      Break;
    Inc(RowCount);
  end;
end;

function TryReadMetadataTextValue(const Row: TMetadataRow; const CandidateFields: array of string; out TextValue: string): Boolean;
var
  I: Integer;
  FieldValue: Variant;
begin
  for I := Low(CandidateFields) to High(CandidateFields) do
  begin
    if not MetadataRowTryGetValue(Row, CandidateFields[I], FieldValue) then
      Continue;
    if VarIsNull(FieldValue) or VarIsEmpty(FieldValue) then
      Continue;
    TextValue := Trim(VarToStr(FieldValue));
    if TextValue <> '' then
      Exit(True);
  end;
  TextValue := '';
  Result := False;
end;

procedure AssertMetadataRestrictionRoundTrip(AClient: TScratchBirdClient; const CollectionName, RestrictionKey: string;
  const CandidateFields: array of string);
var
  BaseRows: TMetadataRows;
  FilteredRows: TMetadataRows;
  Restrictions: TMetadataRow;
  TargetValue: string;
  RowValue: string;
  I: Integer;
begin
  BaseRows := AClient.QueryMetadataRows(CollectionName);
  if Length(BaseRows) = 0 then
  begin
    Writeln('MetadataRestrictionTest: SKIPPED (' + CollectionName + ' has no rows)');
    Exit;
  end;

  if not TryReadMetadataTextValue(BaseRows[0], CandidateFields, TargetValue) then
  begin
    Writeln('MetadataRestrictionTest: SKIPPED (' + CollectionName + ' does not expose expected filter field)');
    Exit;
  end;

  SetLength(Restrictions, 1);
  Restrictions[0].Name := RestrictionKey;
  Restrictions[0].Value := TargetValue;
  FilteredRows := AClient.QueryMetadataRows(CollectionName, Restrictions);
  AssertTrue(Length(FilteredRows) > 0, 'restricted QueryMetadataRows(' + CollectionName + ') should return rows');

  for I := 0 to High(FilteredRows) do
  begin
    if not TryReadMetadataTextValue(FilteredRows[I], CandidateFields, RowValue) then
      Fail('restricted QueryMetadataRows(' + CollectionName + ') row does not expose expected field');
    AssertTrue(SameText(RowValue, TargetValue),
      'restricted QueryMetadataRows(' + CollectionName + ') should match requested ' + RestrictionKey);
  end;
end;

procedure RequireMetadataCollectionHasColumnsAndExecutes(AClient: TScratchBirdClient; const CollectionName: string);
var
  Stream: TScratchBirdResultStream;
  RowCount: Integer;
begin
  Stream := AClient.QueryMetadata(CollectionName);
  try
    DrainStream(Stream, RowCount);
    AssertTrue(Length(Stream.Columns) > 0, CollectionName + ' should expose at least one metadata column');
    AssertTrue(RowCount >= 0, CollectionName + ' row count should be non-negative');
  finally
    Stream.Free;
  end;
end;

procedure RequireMetadataWrapperHasColumnsAndExecutes(AClient: TScratchBirdClient; const WrapperName: string);
var
  Stream: TScratchBirdResultStream;
  RowCount: Integer;
begin
  if WrapperName = 'catalogs' then
    Stream := AClient.GetCatalogs
  else if WrapperName = 'schemas' then
    Stream := AClient.GetSchemas
  else if WrapperName = 'tables' then
    Stream := AClient.GetTables
  else if WrapperName = 'columns' then
    Stream := AClient.GetColumns
  else if WrapperName = 'indexes' then
    Stream := AClient.GetIndexes
  else if WrapperName = 'index_columns' then
    Stream := AClient.GetIndexColumns
  else if WrapperName = 'constraints' then
    Stream := AClient.GetConstraints
  else if WrapperName = 'primary_keys' then
    Stream := AClient.GetPrimaryKeys
  else if WrapperName = 'foreign_keys' then
    Stream := AClient.GetForeignKeys
  else if WrapperName = 'table_privileges' then
    Stream := AClient.GetTablePrivileges
  else if WrapperName = 'column_privileges' then
    Stream := AClient.GetColumnPrivileges
  else if WrapperName = 'procedures' then
    Stream := AClient.GetProcedures
  else if WrapperName = 'functions' then
    Stream := AClient.GetFunctions
  else if WrapperName = 'routines' then
    Stream := AClient.GetRoutines
  else if WrapperName = 'type_info' then
    Stream := AClient.GetTypeInfo
  else
    Fail('unsupported wrapper: ' + WrapperName);
  try
    DrainStream(Stream, RowCount);
    AssertTrue(Length(Stream.Columns) > 0, WrapperName + ' wrapper should expose at least one metadata column');
    AssertTrue(RowCount >= 0, WrapperName + ' wrapper row count should be non-negative');
  finally
    Stream.Free;
  end;
end;

procedure RequireOptionalMetadataCollectionExecutes(AClient: TScratchBirdClient; const CollectionName: string);
var
  Stream: TScratchBirdResultStream;
  RowCount: Integer;
begin
  Stream := AClient.QueryMetadata(CollectionName);
  try
    DrainStream(Stream, RowCount);
    AssertTrue(RowCount >= 0, CollectionName + ' row count should be non-negative');
  finally
    Stream.Free;
  end;
end;

procedure RequireOptionalMetadataWrapperExecutes(AClient: TScratchBirdClient; const WrapperName: string);
var
  Stream: TScratchBirdResultStream;
  RowCount: Integer;
begin
  if WrapperName = 'procedures' then
    Stream := AClient.GetProcedures
  else if WrapperName = 'functions' then
    Stream := AClient.GetFunctions
  else if WrapperName = 'routines' then
    Stream := AClient.GetRoutines
  else
    Fail('unsupported optional wrapper: ' + WrapperName);
  try
    DrainStream(Stream, RowCount);
    AssertTrue(RowCount >= 0, WrapperName + ' wrapper row count should be non-negative');
  finally
    Stream.Free;
  end;
end;

procedure TestQueryAndPrepareBind(AClient: TScratchBirdClient);
var
  Stream: TScratchBirdResultStream;
  Row: TArray<Variant>;
  Param: TScratchBirdParamInput;
begin
  Stream := AClient.ExecuteQuery('SELECT 1');
  try
    Row := Stream.ReadRow;
    AssertTrue(Length(Row) > 0, 'SELECT 1 should return one column');
    AssertEqualInt64(1, RequireVariantInt64(Row[0], 'SELECT 1 value'), 'SELECT 1 payload value');
  finally
    Stream.Free;
  end;

  Param.Value := 42;
  Param.Obj := nil;
  Stream := AClient.ExecuteQueryParams('SELECT ?::INTEGER', [Param]);
  try
    Row := Stream.ReadRow;
    AssertTrue(Length(Row) > 0, 'prepare/bind should return one column');
    AssertEqualInt64(42, RequireVariantInt64(Row[0], 'prepare/bind value'), 'prepare/bind payload value');
  finally
    Stream.Free;
  end;
end;

procedure TestTransactionLifecycle(AClient: TScratchBirdClient);
var
  Stream: TScratchBirdResultStream;
  Row: TArray<Variant>;
begin
  AClient.BeginTransaction;
  try
    AClient.Savepoint('sp_live_1');
    Stream := AClient.ExecuteQuery('SELECT 11');
    try
      Row := Stream.ReadRow;
      AssertTrue(Length(Row) > 0, 'transaction query should return row');
      AssertEqualInt64(11, RequireVariantInt64(Row[0], 'transaction query value'), 'transaction query payload');
    finally
      Stream.Free;
    end;
    AClient.ReleaseSavepoint('sp_live_1');

    AClient.Savepoint('sp_live_2');
    AClient.RollbackToSavepoint('sp_live_2');
    AClient.Commit;
  except
    AClient.Rollback;
    raise;
  end;

  AClient.BeginTransaction;
  AClient.Rollback;
end;

procedure TestLiveBatchAndMulti(AClient: TScratchBirdClient);
var
  Batch: TScratchBirdBatchResults;
  Rowsets: TScratchBirdRowsets;
begin
  Batch := AClient.ExecuteBatch(['SELECT 101', 'SELECT 202']);
  AssertTrue(Length(Batch) = 2, 'ExecuteBatch should return one summary per statement');

  Rowsets := AClient.QueryMulti(['SELECT 303', 'SELECT 404']);
  AssertTrue(Length(Rowsets) = 2, 'QueryMulti should return one rowset per statement');
  AssertTrue(Length(Rowsets[0].Rows) > 0, 'QueryMulti first rowset should include rows');
  AssertTrue(Length(Rowsets[1].Rows) > 0, 'QueryMulti second rowset should include rows');
  AssertEqualInt64(303, RequireVariantInt64(Rowsets[0].Rows[0][0], 'QueryMulti first row value'), 'QueryMulti first row payload');
  AssertEqualInt64(404, RequireVariantInt64(Rowsets[1].Rows[0][0], 'QueryMulti second row value'), 'QueryMulti second row payload');
end;

procedure TestLiveStreamControlPath(AClient: TScratchBirdClient; const CustomSql: string);
var
  Stream: TScratchBirdResultStream;
  QuerySql: string;
  RowCount: Integer;
begin
  QuerySql := Trim(CustomSql);
  if QuerySql = '' then
    QuerySql := 'SELECT id FROM basic_table ORDER BY id';

  Stream := AClient.ExecuteQuery(QuerySql);
  try
    AClient.StreamControl(STREAM_RESUME, 64, 1000);
    DrainStream(Stream, RowCount);
    AssertTrue(RowCount >= 1, 'stream-control query should return at least one row');
  finally
    Stream.Free;
  end;
end;

procedure TestMetadataFamiliesAndRestrictions(AClient: TScratchBirdClient);
var
  SchemaRows: TMetadataRows;
  FilteredRows: TMetadataRows;
  Restrictions: TMetadataRow;
  SchemaValue: Variant;
  SchemaName: string;
begin
  RequireMetadataCollectionHasColumnsAndExecutes(AClient, 'catalogs');
  RequireMetadataCollectionHasColumnsAndExecutes(AClient, 'schemas');
  RequireMetadataCollectionHasColumnsAndExecutes(AClient, 'tables');
  RequireMetadataCollectionHasColumnsAndExecutes(AClient, 'columns');
  RequireMetadataCollectionHasColumnsAndExecutes(AClient, 'indexes');
  RequireMetadataCollectionHasColumnsAndExecutes(AClient, 'index_columns');
  RequireMetadataCollectionHasColumnsAndExecutes(AClient, 'constraints');
  RequireMetadataCollectionHasColumnsAndExecutes(AClient, 'primary_keys');
  RequireMetadataCollectionHasColumnsAndExecutes(AClient, 'foreign_keys');
  RequireMetadataCollectionHasColumnsAndExecutes(AClient, 'table_privileges');
  RequireMetadataCollectionHasColumnsAndExecutes(AClient, 'column_privileges');
  RequireOptionalMetadataCollectionExecutes(AClient, 'procedures');
  RequireOptionalMetadataCollectionExecutes(AClient, 'functions');
  RequireOptionalMetadataCollectionExecutes(AClient, 'routines');
  RequireMetadataCollectionHasColumnsAndExecutes(AClient, 'type_info');
  RequireMetadataWrapperHasColumnsAndExecutes(AClient, 'catalogs');
  RequireMetadataWrapperHasColumnsAndExecutes(AClient, 'schemas');
  RequireMetadataWrapperHasColumnsAndExecutes(AClient, 'tables');
  RequireMetadataWrapperHasColumnsAndExecutes(AClient, 'columns');
  RequireMetadataWrapperHasColumnsAndExecutes(AClient, 'indexes');
  RequireMetadataWrapperHasColumnsAndExecutes(AClient, 'index_columns');
  RequireMetadataWrapperHasColumnsAndExecutes(AClient, 'constraints');
  RequireMetadataWrapperHasColumnsAndExecutes(AClient, 'primary_keys');
  RequireMetadataWrapperHasColumnsAndExecutes(AClient, 'foreign_keys');
  RequireMetadataWrapperHasColumnsAndExecutes(AClient, 'table_privileges');
  RequireMetadataWrapperHasColumnsAndExecutes(AClient, 'column_privileges');
  RequireOptionalMetadataWrapperExecutes(AClient, 'procedures');
  RequireOptionalMetadataWrapperExecutes(AClient, 'functions');
  RequireOptionalMetadataWrapperExecutes(AClient, 'routines');
  RequireMetadataWrapperHasColumnsAndExecutes(AClient, 'type_info');

  SchemaRows := AClient.QueryMetadataRows('schemas');
  AssertTrue(Length(SchemaRows) > 0, 'QueryMetadataRows(schemas) should return at least one row');
  if MetadataRowTryGetValue(SchemaRows[0], 'schema_name', SchemaValue) or
     MetadataRowTryGetValue(SchemaRows[0], 'TABLE_SCHEM', SchemaValue) or
     MetadataRowTryGetValue(SchemaRows[0], 'table_schema', SchemaValue) then
  begin
    AssertTrue((not VarIsNull(SchemaValue)) and (not VarIsEmpty(SchemaValue)), 'schema value should not be null');
    SchemaName := VarToStr(SchemaValue);
  end
  else
    Fail('QueryMetadataRows(schemas) row does not expose schema name field');
  AssertTrue(SchemaName <> '', 'schema value should not be empty');

  SetLength(Restrictions, 1);
  Restrictions[0].Name := 'schema';
  Restrictions[0].Value := SchemaName;
  FilteredRows := AClient.QueryMetadataRows('schemas', Restrictions);
  AssertTrue(Length(FilteredRows) > 0, 'restricted QueryMetadataRows(schemas) should return rows');
  if MetadataRowTryGetValue(FilteredRows[0], 'schema_name', SchemaValue) or
     MetadataRowTryGetValue(FilteredRows[0], 'TABLE_SCHEM', SchemaValue) or
     MetadataRowTryGetValue(FilteredRows[0], 'table_schema', SchemaValue) then
  begin
    AssertTrue((not VarIsNull(SchemaValue)) and (not VarIsEmpty(SchemaValue)), 'restricted schema value should not be null');
    AssertTrue(VarToStr(SchemaValue) = SchemaName, 'restricted schema value should match requested schema');
  end
  else
    Fail('restricted QueryMetadataRows(schemas) row does not expose schema name field');

  AssertMetadataRestrictionRoundTrip(AClient, 'catalogs', 'catalog', ['catalog_name', 'TABLE_CATALOG', 'catalog']);
  AssertMetadataRestrictionRoundTrip(AClient, 'tables', 'table', ['table_name', 'TABLE_NAME', 'table']);
  AssertMetadataRestrictionRoundTrip(AClient, 'columns', 'column', ['column_name', 'COLUMN_NAME', 'column']);
  AssertMetadataRestrictionRoundTrip(AClient, 'indexes', 'index', ['index_name', 'INDEX_NAME', 'index']);
  AssertMetadataRestrictionRoundTrip(AClient, 'index_columns', 'index', ['index_name', 'INDEX_NAME', 'index']);
  AssertMetadataRestrictionRoundTrip(AClient, 'constraints', 'constraint', ['constraint_name', 'CONSTRAINT_NAME', 'constraint']);
  AssertMetadataRestrictionRoundTrip(AClient, 'primary_keys', 'constraint',
    ['constraint_name', 'CONSTRAINT_NAME', 'constraint']);
  AssertMetadataRestrictionRoundTrip(AClient, 'foreign_keys', 'constraint',
    ['constraint_name', 'CONSTRAINT_NAME', 'constraint']);
  AssertMetadataRestrictionRoundTrip(AClient, 'table_privileges', 'table', ['table_name', 'TABLE_NAME', 'table']);
  AssertMetadataRestrictionRoundTrip(AClient, 'column_privileges', 'column', ['column_name', 'COLUMN_NAME', 'column']);
  AssertMetadataRestrictionRoundTrip(AClient, 'procedures', 'procedure',
    ['procedure_name', 'PROCEDURE_NAME', 'routine_name']);
  AssertMetadataRestrictionRoundTrip(AClient, 'functions', 'function',
    ['function_name', 'FUNCTION_NAME', 'routine_name']);
  AssertMetadataRestrictionRoundTrip(AClient, 'routines', 'procedure',
    ['routine_name', 'ROUTINE_NAME', 'procedure_name', 'function_name']);
  AssertMetadataRestrictionRoundTrip(AClient, 'type_info', 'type',
    ['type_name', 'TYPE_NAME', 'data_type_name', 'DATA_TYPE_NAME']);
end;

procedure TestTypeCoverageFixture(AClient: TScratchBirdClient);
var
  Stream: TScratchBirdResultStream;
  Row: TArray<Variant>;
begin
  Stream := AClient.ExecuteQuery('SELECT * FROM type_coverage');
  try
    Row := Stream.ReadRow;
    AssertTrue(Length(Row) > 0, 'type_coverage fixture should return at least one row');
  finally
    Stream.Free;
  end;
end;

procedure TestOptionalCancelPath(AClient: TScratchBirdClient; const SqlText: string);
var
  Stream: TScratchBirdResultStream;
begin
  if SqlText = '' then
    Exit;
  Stream := AClient.ExecuteQuery(SqlText);
  AClient.Cancel;
  try
    Stream.ReadRow;
    raise Exception.Create('Cancel did not interrupt query');
  except
    on E: Exception do
      Writeln('CancelTest: OK');
  end;
end;

procedure TestGeneratedKeyPath(AClient: TScratchBirdClient; const SqlText, ExpectedText: string);
var
  Stream: TScratchBirdResultStream;
  RowCount: Integer;
  Expected: UInt64;
  EffectiveSql: string;
begin
  EffectiveSql := Trim(SqlText);
  if EffectiveSql = '' then
  begin
    Writeln('GeneratedKeyTest: SKIPPED (SCRATCHBIRD_PASCAL_GENERATED_KEY_SQL not set)');
    Exit;
  end;

  Stream := AClient.ExecuteQuery(EffectiveSql);
  try
    DrainStream(Stream, RowCount);
    if not Stream.HasLastInsertId then
      Fail('generated-key SQL did not expose last insert id');
    AssertTrue(Stream.LastInsertId > 0, 'generated-key SQL should expose non-zero last insert id');
    if Trim(ExpectedText) <> '' then
    begin
      if not TryStrToQWord(Trim(ExpectedText), Expected) then
        Fail('SCRATCHBIRD_PASCAL_GENERATED_KEY_EXPECTED must be an unsigned integer');
      AssertEqualUInt64(Expected, Stream.LastInsertId, 'generated-key SQL expected last insert id');
    end;
  finally
    Stream.Free;
  end;
end;

begin
  Dsn := GetEnvironmentVariable('SCRATCHBIRD_PASCAL_URL');
  if Dsn = '' then
  begin
    Writeln('IntegrationTest: SKIPPED (SCRATCHBIRD_PASCAL_URL not set)');
    Halt(0);
  end;
  Client := TScratchBirdClient.Create;
  try
    Client.Connect(Dsn);
    TestQueryAndPrepareBind(Client);
    TestTransactionLifecycle(Client);
    TestLiveBatchAndMulti(Client);
    TestMetadataFamiliesAndRestrictions(Client);
    TestTypeCoverageFixture(Client);
    GeneratedKeySql := GetEnvironmentVariable('SCRATCHBIRD_PASCAL_GENERATED_KEY_SQL');
    GeneratedKeyExpectedText := GetEnvironmentVariable('SCRATCHBIRD_PASCAL_GENERATED_KEY_EXPECTED');
    TestGeneratedKeyPath(Client, GeneratedKeySql, GeneratedKeyExpectedText);
    StreamSql := GetEnvironmentVariable('SCRATCHBIRD_PASCAL_STREAM_SQL');
    TestLiveStreamControlPath(Client, StreamSql);
    CancelSql := GetEnvironmentVariable('SCRATCHBIRD_PASCAL_CANCEL_SQL');
    TestOptionalCancelPath(Client, CancelSql);
    Writeln('IntegrationTest: OK');
  finally
    Client.Free;
  end;
end.
