// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

unit ScratchBird.Errors;

{$mode delphi}
{$H+}

interface

uses
  SysUtils;

type
  TScratchBirdRetryScope = (
    rsNone,
    rsStatement,
    rsReconnect
  );

  EScratchBirdError = class(Exception)
  public
    SQLState: string;
    Detail: string;
    Hint: string;
    constructor CreateWithInfo(const Msg, ASQLState, ADetail, AHint: string);
  end;

  EScratchbirdWarning = class(EScratchBirdError);
  EScratchbirdNoData = class(EScratchBirdError);
  EScratchbirdConnectionError = class(EScratchBirdError);
  EScratchbirdNotSupported = class(EScratchBirdError);
  EScratchbirdDataError = class(EScratchBirdError);
  EScratchbirdIntegrityError = class(EScratchBirdError);
  EScratchbirdAuthError = class(EScratchBirdError);
  EScratchbirdTransactionError = class(EScratchBirdError);
  EScratchbirdSyntaxError = class(EScratchBirdError);
  EScratchbirdResourceError = class(EScratchBirdError);
  EScratchbirdLimitError = class(EScratchBirdError);
  EScratchbirdOperatorInterventionError = class(EScratchBirdError);
  EScratchbirdSystemError = class(EScratchBirdError);
  EScratchbirdInternalError = class(EScratchBirdError);

function MapSqlState(const SQLState: string; const Msg, Detail, Hint: string): EScratchBirdError;
function RetryScopeForSqlState(const SQLState: string): TScratchBirdRetryScope;
function IsRetryableSqlState(const SQLState: string): Boolean;

implementation

constructor EScratchBirdError.CreateWithInfo(const Msg, ASQLState, ADetail, AHint: string);
begin
  inherited Create(Msg);
  SQLState := ASQLState;
  Detail := ADetail;
  Hint := AHint;
end;

function MapSqlState(const SQLState: string; const Msg, Detail, Hint: string): EScratchBirdError;
begin
  if Length(SQLState) = 5 then
  begin
    if SQLState = '01000' then
      Exit(EScratchbirdWarning.CreateWithInfo(Msg, SQLState, Detail, Hint))
    else if SQLState = '02000' then
      Exit(EScratchbirdNoData.CreateWithInfo(Msg, SQLState, Detail, Hint))
    else if (SQLState = '08001') or (SQLState = '08003') or (SQLState = '08004') or
            (SQLState = '08006') or (SQLState = '08P01') then
      Exit(EScratchbirdConnectionError.CreateWithInfo(Msg, SQLState, Detail, Hint))
    else if SQLState = '0A000' then
      Exit(EScratchbirdNotSupported.CreateWithInfo(Msg, SQLState, Detail, Hint))
    else if (SQLState = '22001') or (SQLState = '22003') or (SQLState = '22007') or
            (SQLState = '22012') or (SQLState = '22023') or (SQLState = '22P02') or
            (SQLState = '22P03') then
      Exit(EScratchbirdDataError.CreateWithInfo(Msg, SQLState, Detail, Hint))
    else if (SQLState = '23000') or (SQLState = '23502') or (SQLState = '23503') or
            (SQLState = '23505') or (SQLState = '23514') then
      Exit(EScratchbirdIntegrityError.CreateWithInfo(Msg, SQLState, Detail, Hint))
    else if (SQLState = '28000') or (SQLState = '28P01') then
      Exit(EScratchbirdAuthError.CreateWithInfo(Msg, SQLState, Detail, Hint))
    else if (SQLState = '40001') or (SQLState = '40P01') then
      Exit(EScratchbirdTransactionError.CreateWithInfo(Msg, SQLState, Detail, Hint))
    else if (SQLState = '42501') or (SQLState = '42601') or (SQLState = '42703') or
            (SQLState = '42704') or (SQLState = '42710') or (SQLState = '42883') or
            (SQLState = '42P01') or (SQLState = '42P07') then
      Exit(EScratchbirdSyntaxError.CreateWithInfo(Msg, SQLState, Detail, Hint))
    else if (SQLState = '53P00') or (SQLState = '53100') or (SQLState = '53200') or
            (SQLState = '53300') then
      Exit(EScratchbirdResourceError.CreateWithInfo(Msg, SQLState, Detail, Hint))
    else if SQLState = '54000' then
      Exit(EScratchbirdLimitError.CreateWithInfo(Msg, SQLState, Detail, Hint))
    else if (SQLState = '57014') or (SQLState = '57P01') or (SQLState = '57P03') then
      Exit(EScratchbirdOperatorInterventionError.CreateWithInfo(Msg, SQLState, Detail, Hint))
    else if SQLState = '58000' then
      Exit(EScratchbirdSystemError.CreateWithInfo(Msg, SQLState, Detail, Hint))
    else if SQLState = 'XX000' then
      Exit(EScratchbirdInternalError.CreateWithInfo(Msg, SQLState, Detail, Hint));
  end;
  Result := EScratchBirdError.CreateWithInfo(Msg, SQLState, Detail, Hint);
end;

function RetryScopeForSqlState(const SQLState: string): TScratchBirdRetryScope;
begin
  // ScratchBird's MGA restart contract is boundary-based rather than replay-based:
  //   40xxx conflicts => retry the fresh statement only
  //   08xxx transport/session failures => reconnect or reopen only
  //   57xxx cancellation/operator intervention => no automatic replay
  if (SQLState = '40001') or (SQLState = '40P01') then
    Exit(rsStatement);
  if (Length(SQLState) = 5) and (Copy(SQLState, 1, 2) = '08') then
    Exit(rsReconnect);
  Result := rsNone;
end;

function IsRetryableSqlState(const SQLState: string): Boolean;
begin
  Result := RetryScopeForSqlState(SQLState) <> rsNone;
end;

end.
