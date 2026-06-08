// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

unit ScratchBird.Metadata;

{$mode delphi}
{$H+}

interface

uses
  SysUtils, Classes, Variants, Contnrs, ScratchBird.Errors;

type
  TMetadataField = record
    Name: string;
    Value: Variant;
  end;

  TMetadataRow = TArray<TMetadataField>;
  TMetadataRows = TArray<TMetadataRow>;

  TMetadataSchemaTreeNode = class
  private
    FName: string;
    FPath: string;
    FTerminal: Boolean;
    FChildren: TObjectList;
    function GetChild(Index: Integer): TMetadataSchemaTreeNode;
    function GetChildCount: Integer;
  public
    constructor Create(const AName, APath: string; ATerminal: Boolean);
    destructor Destroy; override;
    function EnsureChild(const AName, APath: string; ATerminal: Boolean): TMetadataSchemaTreeNode;
    function FindDescendantByPath(const APath: string): TMetadataSchemaTreeNode;
    property Name: string read FName;
    property Path: string read FPath;
    property Terminal: Boolean read FTerminal write FTerminal;
    property ChildCount: Integer read GetChildCount;
    property Children[Index: Integer]: TMetadataSchemaTreeNode read GetChild;
  end;

  TMetadataSchemaTree = class
  private
    FDatabase: string;
    FSchemaRoots: TObjectList;
    function GetSchema(Index: Integer): TMetadataSchemaTreeNode;
    function GetSchemaCount: Integer;
  public
    constructor Create;
    destructor Destroy; override;
    function EnsureRoot(const AName, APath: string; ATerminal: Boolean): TMetadataSchemaTreeNode;
    function FindNodeByPath(const APath: string): TMetadataSchemaTreeNode;
    property Database: string read FDatabase write FDatabase;
    property SchemaCount: Integer read GetSchemaCount;
    property Schemas[Index: Integer]: TMetadataSchemaTreeNode read GetSchema;
  end;

function MetadataSchemasQuery: string;
function MetadataTablesQuery: string;
function MetadataColumnsQuery: string;
function MetadataIndexesQuery: string;
function MetadataIndexColumnsQuery: string;
function MetadataConstraintsQuery: string;
function MetadataProceduresQuery: string;
function MetadataFunctionsQuery: string;
function MetadataRoutinesQuery: string;
function MetadataCatalogsQuery: string;
function MetadataPrimaryKeysQuery: string;
function MetadataForeignKeysQuery: string;
function MetadataTablePrivilegesQuery: string;
function MetadataColumnPrivilegesQuery: string;
function MetadataTypeInfoQuery: string;
function NormalizeMetadataCollectionName(const CollectionName: string): string;
function ResolveMetadataCollectionQuery(const CollectionName: string): string;
function MetadataRowTryGetValue(const Row: TMetadataRow; const Key: string; out Value: Variant): Boolean;
function FilterMetadataRowsByRestrictions(const Rows: TMetadataRows; const Restrictions: TMetadataRow;
  const CollectionName: string = 'tables'): TMetadataRows;
function ExpandSchemaPaths(const SchemaPaths: array of string): TArray<string>;
function ListMetadataSchemaPaths(const Rows: TMetadataRows; ExpandParents: Boolean): TArray<string>;
function ExpandSchemaMetadataRows(const Rows: TMetadataRows): TMetadataRows;
function BuildMetadataSchemaTree(const Rows: TMetadataRows; ExpandParents: Boolean;
  const Database: string = ''): TMetadataSchemaTree;

implementation

const
  SCHEMA_FIELD_CANDIDATES: array[0..5] of string = (
    'schema_name',
    'TABLE_SCHEM',
    'table_schem',
    'table_schema',
    'TABLE_SCHEMA',
    'schema'
  );
  METADATA_COLLECTION_ALIASES: array[0..34, 0..1] of string = (
    ('schemas', 'schemas'),
    ('schema', 'schemas'),
    ('tables', 'tables'),
    ('table', 'tables'),
    ('columns', 'columns'),
    ('column', 'columns'),
    ('indexes', 'indexes'),
    ('index', 'indexes'),
    ('index_columns', 'index_columns'),
    ('indexcolumns', 'index_columns'),
    ('constraints', 'constraints'),
    ('constraint', 'constraints'),
    ('procedures', 'procedures'),
    ('procedure', 'procedures'),
    ('functions', 'functions'),
    ('function', 'functions'),
    ('routines', 'routines'),
    ('routine', 'routines'),
    ('catalogs', 'catalogs'),
    ('catalog', 'catalogs'),
    ('primary_keys', 'primary_keys'),
    ('primarykeys', 'primary_keys'),
    ('primarykey', 'primary_keys'),
    ('pk', 'primary_keys'),
    ('foreign_keys', 'foreign_keys'),
    ('foreignkeys', 'foreign_keys'),
    ('foreignkey', 'foreign_keys'),
    ('fk', 'foreign_keys'),
    ('table_privileges', 'table_privileges'),
    ('tableprivileges', 'table_privileges'),
    ('column_privileges', 'column_privileges'),
    ('columnprivileges', 'column_privileges'),
    ('type_info', 'type_info'),
    ('typeinfo', 'type_info'),
    ('types', 'type_info')
  );

type
  TMetadataRestrictionBinding = record
    Aliases: TArray<string>;
    ExpectNull: Boolean;
    Pattern: string;
  end;

function NormalizeCollectionKey(const Value: string): string;
var
  I: Integer;
  Ch: Char;
begin
  Result := '';
  for I := 1 to Length(Value) do
  begin
    Ch := Value[I];
    if ((Ch >= 'a') and (Ch <= 'z')) or ((Ch >= '0') and (Ch <= '9')) then
      Result := Result + Ch;
  end;
end;

function NormalizeMetadataCollectionName(const CollectionName: string): string;
var
  RawName: string;
  Collapsed: string;
  I: Integer;
begin
  RawName := LowerCase(Trim(CollectionName));
  if RawName = '' then
    RawName := 'tables';
  Collapsed := NormalizeCollectionKey(RawName);
  for I := Low(METADATA_COLLECTION_ALIASES) to High(METADATA_COLLECTION_ALIASES) do
  begin
    if (RawName = METADATA_COLLECTION_ALIASES[I, 0]) or
      (Collapsed = NormalizeCollectionKey(METADATA_COLLECTION_ALIASES[I, 0])) then
      Exit(METADATA_COLLECTION_ALIASES[I, 1]);
  end;
  raise EScratchbirdNotSupported.CreateWithInfo(
    'Metadata collection "' + CollectionName + '" is not supported',
    '0A000',
    '',
    ''
  );
end;

function ResolveMetadataCollectionQuery(const CollectionName: string): string;
var
  Normalized: string;
begin
  Normalized := NormalizeMetadataCollectionName(CollectionName);
  if Normalized = 'schemas' then
    Exit(MetadataSchemasQuery);
  if Normalized = 'tables' then
    Exit(MetadataTablesQuery);
  if Normalized = 'columns' then
    Exit(MetadataColumnsQuery);
  if Normalized = 'indexes' then
    Exit(MetadataIndexesQuery);
  if Normalized = 'index_columns' then
    Exit(MetadataIndexColumnsQuery);
  if Normalized = 'constraints' then
    Exit(MetadataConstraintsQuery);
  if Normalized = 'procedures' then
    Exit(MetadataProceduresQuery);
  if Normalized = 'functions' then
    Exit(MetadataFunctionsQuery);
  if Normalized = 'routines' then
    Exit(MetadataRoutinesQuery);
  if Normalized = 'catalogs' then
    Exit(MetadataCatalogsQuery);
  if Normalized = 'primary_keys' then
    Exit(MetadataPrimaryKeysQuery);
  if Normalized = 'foreign_keys' then
    Exit(MetadataForeignKeysQuery);
  if Normalized = 'table_privileges' then
    Exit(MetadataTablePrivilegesQuery);
  if Normalized = 'column_privileges' then
    Exit(MetadataColumnPrivilegesQuery);
  if Normalized = 'type_info' then
    Exit(MetadataTypeInfoQuery);
  raise EScratchbirdNotSupported.CreateWithInfo(
    'Metadata collection "' + CollectionName + '" is not supported',
    '0A000',
    '',
    ''
  );
end;

function AppendUniqueString(var Values: TArray<string>; Seen: TStringList; const Value: string): Boolean;
var
  Count: Integer;
begin
  if Seen.IndexOf(Value) >= 0 then
    Exit(False);
  Seen.Add(Value);
  Count := Length(Values);
  SetLength(Values, Count + 1);
  Values[Count] := Value;
  Result := True;
end;

function MarkSeen(Seen: TStringList; const Value: string): Boolean;
begin
  if Seen.IndexOf(Value) >= 0 then
    Exit(False);
  Seen.Add(Value);
  Result := True;
end;

function IsSchemaFieldCandidate(const Name: string): Boolean;
var
  I: Integer;
begin
  for I := Low(SCHEMA_FIELD_CANDIDATES) to High(SCHEMA_FIELD_CANDIDATES) do
  begin
    if SameText(Name, SCHEMA_FIELD_CANDIDATES[I]) then
      Exit(True);
  end;
  Result := False;
end;

function SplitSchemaPath(const Value: string): TArray<string>;
var
  I, StartIndex, Count: Integer;
  Segment: string;
begin
  Result := nil;
  SetLength(Result, 0);
  StartIndex := 1;
  for I := 1 to Length(Value) + 1 do
  begin
    if (I > Length(Value)) or (Value[I] = '.') then
    begin
      Segment := Trim(Copy(Value, StartIndex, I - StartIndex));
      if Segment <> '' then
      begin
        Count := Length(Result);
        SetLength(Result, Count + 1);
        Result[Count] := Segment;
      end;
      StartIndex := I + 1;
    end;
  end;
end;

function NormalizeSchemaPath(const Value: string; out Normalized: string): Boolean;
var
  Parts: TArray<string>;
  I: Integer;
begin
  Parts := SplitSchemaPath(Value);
  if Length(Parts) = 0 then
  begin
    Normalized := '';
    Exit(False);
  end;
  Normalized := Parts[0];
  for I := 1 to High(Parts) do
    Normalized := Normalized + '.' + Parts[I];
  Result := True;
end;

function CloneMetadataRow(const Row: TMetadataRow): TMetadataRow;
var
  I: Integer;
begin
  Result := nil;
  SetLength(Result, Length(Row));
  for I := 0 to High(Row) do
  begin
    Result[I].Name := Row[I].Name;
    Result[I].Value := Row[I].Value;
  end;
end;

function NormalizeMetadataIdentifier(const Value: string): string;
begin
  Result := NormalizeCollectionKey(Value);
end;

function MetadataRowTryGetValue(const Row: TMetadataRow; const Key: string; out Value: Variant): Boolean;
var
  I: Integer;
begin
  for I := 0 to High(Row) do
  begin
    if SameText(Row[I].Name, Key) then
    begin
      Value := Row[I].Value;
      Exit(True);
    end;
  end;
  Value := Null;
  Result := False;
end;

function MetadataRowTryGetValueByAlias(const Row: TMetadataRow; const Alias: string; out Value: Variant): Boolean;
var
  I: Integer;
  Target, Candidate: string;
begin
  Target := NormalizeMetadataIdentifier(Alias);
  if Target = '' then
  begin
    Value := Null;
    Exit(False);
  end;

  for I := 0 to High(Row) do
  begin
    Candidate := NormalizeMetadataIdentifier(Row[I].Name);
    if Candidate = Target then
    begin
      Value := Row[I].Value;
      Exit(True);
    end;
  end;

  Value := Null;
  Result := False;
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

function MetadataRestrictionAliases(const Key: string): TArray<string>;
var
  Canonical: string;
begin
  Canonical := NormalizeMetadataIdentifier(Key);
  if Canonical = 'catalog' then
    Result := StringArray(['catalog_name', 'table_catalog', 'table_cat', 'catalog'])
  else if Canonical = 'schema' then
    Result := StringArray(['schema_name', 'table_schema', 'table_schem', 'schema'])
  else if Canonical = 'table' then
    Result := StringArray(['table_name', 'table', 'relname'])
  else if Canonical = 'column' then
    Result := StringArray(['column_name', 'column'])
  else if Canonical = 'index' then
    Result := StringArray(['index_name', 'index'])
  else if Canonical = 'constraint' then
    Result := StringArray(['constraint_name', 'constraint'])
  else if Canonical = 'procedure' then
    Result := StringArray(['procedure_name', 'routine_name', 'procedure'])
  else if Canonical = 'function' then
    Result := StringArray(['function_name', 'routine_name', 'function'])
  else if Canonical = 'type' then
    Result := StringArray(['type_name', 'data_type_name', 'data_type', 'udt_name'])
  else
    Result := nil;

  if Canonical <> '' then
  begin
    SetLength(Result, Length(Result) + 1);
    Result[High(Result)] := Canonical;
  end;
end;

function MetadataCollectionRestrictionKeys(const CollectionName: string): TArray<string>;
var
  Resolved: string;
begin
  Result := nil;
  Resolved := '';
  try
    Resolved := NormalizeMetadataCollectionName(CollectionName);
  except
    on EScratchbirdNotSupported do
      Exit(nil);
  end;

  if Resolved = 'catalogs' then
    Exit(StringArray(['catalog']));
  if Resolved = 'schemas' then
    Exit(StringArray(['catalog', 'schema']));
  if Resolved = 'tables' then
    Exit(StringArray(['catalog', 'schema', 'table', 'type']));
  if Resolved = 'columns' then
    Exit(StringArray(['catalog', 'schema', 'table', 'column', 'type']));
  if Resolved = 'indexes' then
    Exit(StringArray(['catalog', 'schema', 'table', 'index']));
  if Resolved = 'index_columns' then
    Exit(StringArray(['catalog', 'schema', 'table', 'index', 'column']));
  if Resolved = 'constraints' then
    Exit(StringArray(['catalog', 'schema', 'table', 'constraint']));
  if Resolved = 'primary_keys' then
    Exit(StringArray(['catalog', 'schema', 'table', 'constraint']));
  if Resolved = 'foreign_keys' then
    Exit(StringArray(['catalog', 'schema', 'table', 'constraint']));
  if Resolved = 'table_privileges' then
    Exit(StringArray(['catalog', 'schema', 'table']));
  if Resolved = 'column_privileges' then
    Exit(StringArray(['catalog', 'schema', 'table', 'column']));
  if Resolved = 'procedures' then
    Exit(StringArray(['catalog', 'schema', 'procedure']));
  if Resolved = 'functions' then
    Exit(StringArray(['catalog', 'schema', 'function']));
  if Resolved = 'routines' then
    Exit(StringArray(['catalog', 'schema', 'procedure', 'function']));
  if Resolved = 'type_info' then
    Exit(StringArray(['type']));
end;

function BuildAllowedRestrictionAliases(const CollectionName: string): TStringList;
var
  RestrictionKeys, Aliases: TArray<string>;
  I, J: Integer;
  Normalized: string;
begin
  Result := TStringList.Create;
  Result.CaseSensitive := True;
  Result.Sorted := False;
  RestrictionKeys := MetadataCollectionRestrictionKeys(CollectionName);
  for I := 0 to High(RestrictionKeys) do
  begin
    Aliases := MetadataRestrictionAliases(RestrictionKeys[I]);
    for J := 0 to High(Aliases) do
    begin
      Normalized := NormalizeMetadataIdentifier(Aliases[J]);
      if (Normalized = '') or (Result.IndexOf(Normalized) >= 0) then
        Continue;
      Result.Add(Normalized);
    end;
  end;
end;

function IsAllowedAlias(AllowedAliases: TStringList; const Alias: string): Boolean;
var
  Normalized: string;
begin
  if (AllowedAliases = nil) or (AllowedAliases.Count = 0) then
    Exit(True);
  Normalized := NormalizeMetadataIdentifier(Alias);
  if Normalized = '' then
    Exit(False);
  Result := AllowedAliases.IndexOf(Normalized) >= 0;
end;

function RowContainsAnyAlias(const Row: TMetadataRow; const Aliases: TArray<string>): Boolean;
var
  I: Integer;
  Value: Variant;
begin
  for I := 0 to High(Aliases) do
  begin
    if MetadataRowTryGetValueByAlias(Row, Aliases[I], Value) then
      Exit(True);
  end;
  Result := False;
end;

function AnyRowContainsAnyAlias(const Rows: TMetadataRows; const Aliases: TArray<string>): Boolean;
var
  I: Integer;
begin
  for I := 0 to High(Rows) do
  begin
    if RowContainsAnyAlias(Rows[I], Aliases) then
      Exit(True);
  end;
  Result := False;
end;

function RestrictionExpectsNull(const Value: Variant; out Pattern: string): Boolean;
begin
  if VarIsNull(Value) or VarIsEmpty(Value) then
  begin
    Pattern := '';
    Exit(True);
  end;

  Pattern := Trim(VarToStr(Value));
  if Pattern = '' then
    Exit(False);
  Result := SameText(Pattern, 'null');
end;

function ContainsWildcard(const Pattern: string): Boolean;
begin
  Result := (Pos('%', Pattern) > 0) or (Pos('_', Pattern) > 0);
end;

function MatchesWildcardPattern(const Value, Pattern: string): Boolean;
var
  ValueLower, PatternLower: string;
  ValueIdx, PatternIdx: Integer;
  LastPercentIdx, RetryValueIdx: Integer;
begin
  ValueLower := LowerCase(Value);
  PatternLower := LowerCase(Pattern);
  ValueIdx := 1;
  PatternIdx := 1;
  LastPercentIdx := 0;
  RetryValueIdx := 0;

  while ValueIdx <= Length(ValueLower) do
  begin
    if (PatternIdx <= Length(PatternLower)) and
       ((PatternLower[PatternIdx] = '_') or (PatternLower[PatternIdx] = ValueLower[ValueIdx])) then
    begin
      Inc(ValueIdx);
      Inc(PatternIdx);
      Continue;
    end;

    if (PatternIdx <= Length(PatternLower)) and (PatternLower[PatternIdx] = '%') then
    begin
      LastPercentIdx := PatternIdx;
      Inc(PatternIdx);
      RetryValueIdx := ValueIdx;
      Continue;
    end;

    if LastPercentIdx > 0 then
    begin
      PatternIdx := LastPercentIdx + 1;
      Inc(RetryValueIdx);
      ValueIdx := RetryValueIdx;
      Continue;
    end;

    Exit(False);
  end;

  while (PatternIdx <= Length(PatternLower)) and (PatternLower[PatternIdx] = '%') do
    Inc(PatternIdx);

  Result := PatternIdx > Length(PatternLower);
end;

function MatchesRestrictionPattern(const Value, Pattern: string): Boolean;
begin
  if not ContainsWildcard(Pattern) then
    Exit(SameText(Value, Pattern));
  Result := MatchesWildcardPattern(Value, Pattern);
end;

function BuildRestrictionBindings(const Rows: TMetadataRows; const Restrictions: TMetadataRow;
  const CollectionName: string): TArray<TMetadataRestrictionBinding>;
var
  AllowedAliases, AliasSet: TStringList;
  I, J, BindingCount: Integer;
  Restriction: TMetadataField;
  RawAliases, EffectiveAliases: TArray<string>;
  AliasId: string;
  Pattern: string;
  ExpectsNull: Boolean;
begin
  Result := nil;
  SetLength(Result, 0);
  if Length(Restrictions) = 0 then
    Exit;

  AllowedAliases := BuildAllowedRestrictionAliases(CollectionName);
  try
    for I := 0 to High(Restrictions) do
    begin
      Restriction := Restrictions[I];
      if NormalizeMetadataIdentifier(Restriction.Name) = '' then
        Continue;

      RawAliases := MetadataRestrictionAliases(Restriction.Name);
      if Length(RawAliases) = 0 then
        Continue;

      AliasSet := TStringList.Create;
      try
        AliasSet.CaseSensitive := True;
        AliasSet.Sorted := False;
        for J := 0 to High(RawAliases) do
        begin
          AliasId := NormalizeMetadataIdentifier(RawAliases[J]);
          if AliasId = '' then
            Continue;
          if not IsAllowedAlias(AllowedAliases, AliasId) then
            Continue;
          if AliasSet.IndexOf(AliasId) >= 0 then
            Continue;
          AliasSet.Add(AliasId);
        end;

        SetLength(EffectiveAliases, AliasSet.Count);
        for J := 0 to AliasSet.Count - 1 do
          EffectiveAliases[J] := AliasSet[J];
      finally
        AliasSet.Free;
      end;

      if Length(EffectiveAliases) = 0 then
        Continue;
      if not AnyRowContainsAnyAlias(Rows, EffectiveAliases) then
        Continue;

      ExpectsNull := RestrictionExpectsNull(Restriction.Value, Pattern);
      if (not ExpectsNull) and (Pattern = '') then
        Continue;

      BindingCount := Length(Result);
      SetLength(Result, BindingCount + 1);
      Result[BindingCount].Aliases := EffectiveAliases;
      Result[BindingCount].ExpectNull := ExpectsNull;
      Result[BindingCount].Pattern := Pattern;
    end;
  finally
    AllowedAliases.Free;
  end;
end;

function RowMatchesRestrictionBindings(const Row: TMetadataRow;
  const Bindings: TArray<TMetadataRestrictionBinding>): Boolean;
var
  I, J: Integer;
  Binding: TMetadataRestrictionBinding;
  Value: Variant;
  Matched: Boolean;
begin
  for I := 0 to High(Bindings) do
  begin
    Binding := Bindings[I];
    Matched := False;
    for J := 0 to High(Binding.Aliases) do
    begin
      if not MetadataRowTryGetValueByAlias(Row, Binding.Aliases[J], Value) then
        Continue;
      if Binding.ExpectNull then
      begin
        if VarIsNull(Value) or VarIsEmpty(Value) then
        begin
          Matched := True;
          Break;
        end;
        Continue;
      end;

      if VarIsNull(Value) or VarIsEmpty(Value) then
        Continue;
      if MatchesRestrictionPattern(VarToStr(Value), Binding.Pattern) then
      begin
        Matched := True;
        Break;
      end;
    end;

    if not Matched then
      Exit(False);
  end;

  Result := True;
end;

function FilterMetadataRowsByRestrictions(const Rows: TMetadataRows; const Restrictions: TMetadataRow;
  const CollectionName: string): TMetadataRows;
var
  Bindings: TArray<TMetadataRestrictionBinding>;
  I, Count: Integer;
begin
  if Length(Restrictions) = 0 then
    Exit(Rows);

  Bindings := BuildRestrictionBindings(Rows, Restrictions, CollectionName);
  if Length(Bindings) = 0 then
    Exit(Rows);

  Result := nil;
  SetLength(Result, 0);
  for I := 0 to High(Rows) do
  begin
    if not RowMatchesRestrictionBindings(Rows[I], Bindings) then
      Continue;
    Count := Length(Result);
    SetLength(Result, Count + 1);
    Result[Count] := CloneMetadataRow(Rows[I]);
  end;
end;

function TryReadSchemaPath(const Row: TMetadataRow; out SchemaPath: string): Boolean;
var
  I: Integer;
  Value: Variant;
begin
  for I := Low(SCHEMA_FIELD_CANDIDATES) to High(SCHEMA_FIELD_CANDIDATES) do
  begin
    if not MetadataRowTryGetValue(Row, SCHEMA_FIELD_CANDIDATES[I], Value) then
      Continue;
    if VarIsNull(Value) or VarIsEmpty(Value) then
      Continue;
    if NormalizeSchemaPath(VarToStr(Value), SchemaPath) then
      Exit(True);
  end;
  SchemaPath := '';
  Result := False;
end;

function CreateSyntheticSchemaRow(const Sample: TMetadataRow; const SchemaPath: string): TMetadataRow;
var
  I, Count: Integer;
  AssignedSchemaField: Boolean;
begin
  Result := nil;
  SetLength(Result, Length(Sample));
  for I := 0 to High(Sample) do
  begin
    Result[I].Name := Sample[I].Name;
    Result[I].Value := Null;
  end;

  AssignedSchemaField := False;
  for I := 0 to High(Result) do
  begin
    if not IsSchemaFieldCandidate(Result[I].Name) then
      Continue;
    Result[I].Value := SchemaPath;
    AssignedSchemaField := True;
  end;

  if not AssignedSchemaField then
  begin
    Count := Length(Result);
    SetLength(Result, Count + 1);
    Result[Count].Name := 'schema_name';
    Result[Count].Value := SchemaPath;
  end;
end;

function MetadataSchemasQuery: string;
begin
  Result := 'SELECT schema_id, schema_name, owner_id, default_tablespace_id FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name';
end;

function MetadataTablesQuery: string;
begin
  Result := 'SELECT t.table_id, t.schema_id, s.schema_name, s.schema_name AS table_schema, s.schema_name AS table_schem, t.table_name, t.table_type, t.owner_id FROM sys.tables t LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE t.is_valid = 1 AND (s.schema_id IS NULL OR s.is_valid = 1) ORDER BY s.schema_name, t.table_name';
end;

function MetadataColumnsQuery: string;
begin
  Result := 'SELECT c.column_id, c.table_id, t.table_name, s.schema_name, s.schema_name AS table_schema, s.schema_name AS table_schem, c.column_name, c.data_type_id, c.data_type_name, c.ordinal_position, c.is_nullable, c.default_value, c.domain_id, c.collation_id, c.charset_id, c.is_identity, c.is_generated, c.generation_expression FROM sys.columns c LEFT JOIN sys.tables t ON t.table_id = c.table_id LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE c.is_valid = 1 AND (t.table_id IS NULL OR t.is_valid = 1) AND (s.schema_id IS NULL OR s.is_valid = 1) ORDER BY s.schema_name, t.table_name, c.ordinal_position';
end;

function MetadataIndexesQuery: string;
begin
  Result := 'SELECT i.index_id, i.table_id, t.table_name, s.schema_name, s.schema_name AS table_schema, s.schema_name AS table_schem, i.index_name, i.index_type, i.is_unique FROM sys.indexes i LEFT JOIN sys.tables t ON t.table_id = i.table_id LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE i.is_valid = 1 AND (t.table_id IS NULL OR t.is_valid = 1) AND (s.schema_id IS NULL OR s.is_valid = 1) ORDER BY s.schema_name, t.table_name, i.index_name';
end;

function MetadataIndexColumnsQuery: string;
begin
  Result := 'SELECT ic.index_id, i.index_name, i.table_id, t.table_name, s.schema_name, s.schema_name AS table_schema, s.schema_name AS table_schem, ic.column_id, ic.column_name, ic.ordinal_position, ic.is_included FROM sys.index_columns ic LEFT JOIN sys.indexes i ON i.index_id = ic.index_id LEFT JOIN sys.tables t ON t.table_id = i.table_id LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE (i.index_id IS NULL OR i.is_valid = 1) AND (t.table_id IS NULL OR t.is_valid = 1) AND (s.schema_id IS NULL OR s.is_valid = 1) ORDER BY s.schema_name, t.table_name, i.index_name, ic.ordinal_position';
end;

function MetadataConstraintsQuery: string;
begin
  Result := 'SELECT c.constraint_id, c.table_id, t.table_name, s.schema_name, s.schema_name AS table_schema, s.schema_name AS table_schem, c.constraint_name, c.constraint_type FROM sys.constraints c LEFT JOIN sys.tables t ON t.table_id = c.table_id LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE c.is_valid = 1 AND (t.table_id IS NULL OR t.is_valid = 1) AND (s.schema_id IS NULL OR s.is_valid = 1) ORDER BY s.schema_name, t.table_name, c.constraint_name';
end;

function MetadataProceduresQuery: string;
begin
  Result := 'SELECT routine_schema AS schema_id, routine_schema AS schema_name, routine_schema AS table_schema, routine_schema AS table_schem, routine_name AS procedure_name, routine_name AS routine_name, routine_name AS specific_name, routine_type FROM information_schema.routines WHERE lower(routine_type) = ''procedure'' ORDER BY schema_name, procedure_name';
end;

function MetadataFunctionsQuery: string;
begin
  Result := 'SELECT routine_schema AS schema_id, routine_schema AS schema_name, routine_schema AS table_schema, routine_schema AS table_schem, routine_name AS function_name, routine_name AS routine_name, routine_name AS specific_name, routine_type FROM information_schema.routines WHERE lower(routine_type) = ''function'' ORDER BY schema_name, function_name';
end;

function MetadataRoutinesQuery: string;
begin
  Result := 'SELECT routine_schema AS schema_id, routine_schema AS schema_name, routine_schema AS table_schema, routine_schema AS table_schem, routine_name AS routine_name, routine_name AS specific_name, routine_type FROM information_schema.routines WHERE lower(routine_type) = ''procedure'' UNION ALL SELECT routine_schema AS schema_id, routine_schema AS schema_name, routine_schema AS table_schema, routine_schema AS table_schem, routine_name AS routine_name, routine_name AS specific_name, routine_type FROM information_schema.routines WHERE lower(routine_type) = ''function'' ORDER BY schema_name, routine_name';
end;

function MetadataCatalogsQuery: string;
begin
  Result := 'SELECT schema_id AS catalog_id, schema_name AS catalog_name FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name';
end;

function MetadataPrimaryKeysQuery: string;
begin
  Result := 'SELECT c.constraint_id, c.table_id, t.table_name, s.schema_name, s.schema_name AS table_schema, s.schema_name AS table_schem, c.constraint_name, c.constraint_type FROM sys.constraints c LEFT JOIN sys.tables t ON t.table_id = c.table_id LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE c.is_valid = 1 AND (t.table_id IS NULL OR t.is_valid = 1) AND (s.schema_id IS NULL OR s.is_valid = 1) AND lower(constraint_type) IN (''primary key'', ''primary'') ORDER BY s.schema_name, t.table_name, c.constraint_name';
end;

function MetadataForeignKeysQuery: string;
begin
  Result := 'SELECT c.constraint_id, c.table_id, t.table_name, s.schema_name, s.schema_name AS table_schema, s.schema_name AS table_schem, c.constraint_name, c.constraint_type FROM sys.constraints c LEFT JOIN sys.tables t ON t.table_id = c.table_id LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE c.is_valid = 1 AND (t.table_id IS NULL OR t.is_valid = 1) AND (s.schema_id IS NULL OR s.is_valid = 1) AND lower(constraint_type) IN (''foreign key'', ''foreign'') ORDER BY s.schema_name, t.table_name, c.constraint_name';
end;

function MetadataTablePrivilegesQuery: string;
begin
  Result := 'SELECT t.table_id, t.table_name, s.schema_name, s.schema_name AS table_schema, s.schema_name AS table_schem, t.owner_id AS grantor_id, t.owner_id AS grantee_id, ''ALL'' AS privilege_type FROM sys.tables t LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE t.is_valid = 1 AND (s.schema_id IS NULL OR s.is_valid = 1) ORDER BY s.schema_name, t.table_name';
end;

function MetadataColumnPrivilegesQuery: string;
begin
  Result := 'SELECT c.table_id, c.column_id, t.table_name, s.schema_name, s.schema_name AS table_schema, s.schema_name AS table_schem, c.column_name, ''ALL'' AS privilege_type FROM sys.columns c LEFT JOIN sys.tables t ON t.table_id = c.table_id LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE c.is_valid = 1 AND (t.table_id IS NULL OR t.is_valid = 1) AND (s.schema_id IS NULL OR s.is_valid = 1) ORDER BY s.schema_name, t.table_name, c.ordinal_position';
end;

function MetadataTypeInfoQuery: string;
begin
  Result := 'SELECT DISTINCT data_type_id, data_type_name, data_type_name AS type_name, data_type_name AS data_type FROM sys.columns WHERE is_valid = 1 ORDER BY data_type_name';
end;

function ExpandSchemaPaths(const SchemaPaths: array of string): TArray<string>;
var
  Seen: TStringList;
  Parts: TArray<string>;
  NormalizedPath: string;
  CurrentPath: string;
  I, J: Integer;
begin
  Result := nil;
  SetLength(Result, 0);
  Seen := TStringList.Create;
  try
    Seen.CaseSensitive := True;
    Seen.Sorted := False;
    for I := 0 to High(SchemaPaths) do
    begin
      if not NormalizeSchemaPath(SchemaPaths[I], NormalizedPath) then
        Continue;
      Parts := SplitSchemaPath(NormalizedPath);
      if Length(Parts) = 0 then
        Continue;
      CurrentPath := '';
      for J := 0 to High(Parts) do
      begin
        if CurrentPath <> '' then
          CurrentPath := CurrentPath + '.';
        CurrentPath := CurrentPath + Parts[J];
        AppendUniqueString(Result, Seen, CurrentPath);
      end;
    end;
  finally
    Seen.Free;
  end;
end;

function ListMetadataSchemaPaths(const Rows: TMetadataRows; ExpandParents: Boolean): TArray<string>;
var
  Seen: TStringList;
  BasePaths: TArray<string>;
  SchemaPath: string;
  I: Integer;
begin
  SetLength(BasePaths, 0);
  Seen := TStringList.Create;
  try
    Seen.CaseSensitive := True;
    Seen.Sorted := False;
    for I := 0 to High(Rows) do
    begin
      if not TryReadSchemaPath(Rows[I], SchemaPath) then
        Continue;
      AppendUniqueString(BasePaths, Seen, SchemaPath);
    end;
  finally
    Seen.Free;
  end;

  if ExpandParents then
    Result := ExpandSchemaPaths(BasePaths)
  else
    Result := BasePaths;
end;

function ExpandSchemaMetadataRows(const Rows: TMetadataRows): TMetadataRows;
var
  Seen: TStringList;
  SchemaPath: string;
  Parts: TArray<string>;
  CurrentPath: string;
  RowCount, I, J: Integer;
begin
  Result := nil;
  SetLength(Result, 0);
  Seen := TStringList.Create;
  try
    Seen.CaseSensitive := True;
    Seen.Sorted := False;

    for I := 0 to High(Rows) do
    begin
      if not TryReadSchemaPath(Rows[I], SchemaPath) then
      begin
        RowCount := Length(Result);
        SetLength(Result, RowCount + 1);
        Result[RowCount] := CloneMetadataRow(Rows[I]);
        Continue;
      end;

      Parts := SplitSchemaPath(SchemaPath);
      if Length(Parts) = 0 then
      begin
        RowCount := Length(Result);
        SetLength(Result, RowCount + 1);
        Result[RowCount] := CloneMetadataRow(Rows[I]);
        Continue;
      end;

      CurrentPath := '';
      for J := 0 to High(Parts) do
      begin
        if CurrentPath <> '' then
          CurrentPath := CurrentPath + '.';
        CurrentPath := CurrentPath + Parts[J];
        if not MarkSeen(Seen, CurrentPath) then
          Continue;
        RowCount := Length(Result);
        SetLength(Result, RowCount + 1);
        if J = High(Parts) then
          Result[RowCount] := CloneMetadataRow(Rows[I])
        else
          Result[RowCount] := CreateSyntheticSchemaRow(Rows[I], CurrentPath);
      end;
    end;
  finally
    Seen.Free;
  end;
end;

constructor TMetadataSchemaTreeNode.Create(const AName, APath: string; ATerminal: Boolean);
begin
  inherited Create;
  FName := AName;
  FPath := APath;
  FTerminal := ATerminal;
  FChildren := TObjectList.Create(True);
end;

destructor TMetadataSchemaTreeNode.Destroy;
begin
  FChildren.Free;
  inherited Destroy;
end;

function TMetadataSchemaTreeNode.GetChild(Index: Integer): TMetadataSchemaTreeNode;
begin
  Result := TMetadataSchemaTreeNode(FChildren[Index]);
end;

function TMetadataSchemaTreeNode.GetChildCount: Integer;
begin
  Result := FChildren.Count;
end;

function TMetadataSchemaTreeNode.EnsureChild(const AName, APath: string; ATerminal: Boolean): TMetadataSchemaTreeNode;
var
  I: Integer;
  Child: TMetadataSchemaTreeNode;
begin
  for I := 0 to FChildren.Count - 1 do
  begin
    Child := TMetadataSchemaTreeNode(FChildren[I]);
    if Child.Path = APath then
    begin
      if ATerminal then
        Child.Terminal := True;
      Exit(Child);
    end;
  end;

  Result := TMetadataSchemaTreeNode.Create(AName, APath, ATerminal);
  FChildren.Add(Result);
end;

function TMetadataSchemaTreeNode.FindDescendantByPath(const APath: string): TMetadataSchemaTreeNode;
var
  I: Integer;
begin
  if FPath = APath then
    Exit(Self);

  for I := 0 to FChildren.Count - 1 do
  begin
    Result := TMetadataSchemaTreeNode(FChildren[I]).FindDescendantByPath(APath);
    if Result <> nil then
      Exit;
  end;
  Result := nil;
end;

constructor TMetadataSchemaTree.Create;
begin
  inherited Create;
  FSchemaRoots := TObjectList.Create(True);
end;

destructor TMetadataSchemaTree.Destroy;
begin
  FSchemaRoots.Free;
  inherited Destroy;
end;

function TMetadataSchemaTree.GetSchema(Index: Integer): TMetadataSchemaTreeNode;
begin
  Result := TMetadataSchemaTreeNode(FSchemaRoots[Index]);
end;

function TMetadataSchemaTree.GetSchemaCount: Integer;
begin
  Result := FSchemaRoots.Count;
end;

function TMetadataSchemaTree.EnsureRoot(const AName, APath: string; ATerminal: Boolean): TMetadataSchemaTreeNode;
var
  I: Integer;
  Node: TMetadataSchemaTreeNode;
begin
  for I := 0 to FSchemaRoots.Count - 1 do
  begin
    Node := TMetadataSchemaTreeNode(FSchemaRoots[I]);
    if Node.Path = APath then
    begin
      if ATerminal then
        Node.Terminal := True;
      Exit(Node);
    end;
  end;

  Result := TMetadataSchemaTreeNode.Create(AName, APath, ATerminal);
  FSchemaRoots.Add(Result);
end;

function TMetadataSchemaTree.FindNodeByPath(const APath: string): TMetadataSchemaTreeNode;
var
  I: Integer;
begin
  for I := 0 to FSchemaRoots.Count - 1 do
  begin
    Result := TMetadataSchemaTreeNode(FSchemaRoots[I]).FindDescendantByPath(APath);
    if Result <> nil then
      Exit;
  end;
  Result := nil;
end;

function BuildMetadataSchemaTree(const Rows: TMetadataRows; ExpandParents: Boolean;
  const Database: string): TMetadataSchemaTree;
var
  BasePaths, ExpandedPaths: TArray<string>;
  TerminalPaths: TStringList;
  Parts: TArray<string>;
  CurrentPath: string;
  ParentNode, Node: TMetadataSchemaTreeNode;
  I, J: Integer;
begin
  BasePaths := ListMetadataSchemaPaths(Rows, False);
  if ExpandParents then
    ExpandedPaths := ExpandSchemaPaths(BasePaths)
  else
    ExpandedPaths := BasePaths;

  TerminalPaths := TStringList.Create;
  try
    TerminalPaths.CaseSensitive := True;
    TerminalPaths.Sorted := False;
    if ExpandParents then
    begin
      for I := 0 to High(ExpandedPaths) do
        MarkSeen(TerminalPaths, ExpandedPaths[I]);
    end
    else
    begin
      for I := 0 to High(BasePaths) do
        MarkSeen(TerminalPaths, BasePaths[I]);
    end;

    Result := TMetadataSchemaTree.Create;
    Result.Database := Trim(Database);

    for I := 0 to High(ExpandedPaths) do
    begin
      Parts := SplitSchemaPath(ExpandedPaths[I]);
      if Length(Parts) = 0 then
        Continue;

      CurrentPath := '';
      ParentNode := nil;
      for J := 0 to High(Parts) do
      begin
        if CurrentPath <> '' then
          CurrentPath := CurrentPath + '.';
        CurrentPath := CurrentPath + Parts[J];

        if ParentNode = nil then
          Node := Result.EnsureRoot(Parts[J], CurrentPath, TerminalPaths.IndexOf(CurrentPath) >= 0)
        else
          Node := ParentNode.EnsureChild(Parts[J], CurrentPath, TerminalPaths.IndexOf(CurrentPath) >= 0);
        ParentNode := Node;
      end;
    end;
  finally
    TerminalPaths.Free;
  end;
end;

end.
