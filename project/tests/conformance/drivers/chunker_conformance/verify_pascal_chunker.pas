// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// Pascal verifier for the cross-driver statement-chunker fixture (cases.json).
// Exercises the SHARED canonical chunker that the Pascal driver tool uses
// (drivers/driver/pascal/src/ScratchBird.Chunker.pas), so this test proves the
// real implementation — not a copy — reproduces the fixture.
//
// Build (from repo root):
//   fpc -Mdelphi -Fu project/drivers/driver/pascal/src \
//       project/tests/conformance/drivers/chunker_conformance/verify_pascal_chunker.pas \
//       -o/tmp/verify_pascal_chunker
//   /tmp/verify_pascal_chunker project/tests/conformance/drivers/chunker_conformance/cases.json
//
// Exit 0 = all cases pass.
program verify_pascal_chunker;

{$mode delphi}
{$H+}

uses
  SysUtils, Classes, fpjson, jsonparser, ScratchBird.Chunker;

function ReadAll(const Path: string): string;
var
  S: TStringStream;
  F: TFileStream;
begin
  F := TFileStream.Create(Path, fmOpenRead or fmShareDenyNone);
  S := TStringStream.Create('');
  try
    S.CopyFrom(F, 0);
    Result := S.DataString;
  finally
    S.Free;
    F.Free;
  end;
end;

var
  CasesPath: string;
  Data: TJSONData;
  Root, CaseObj: TJSONObject;
  CasesArr, ExpectedArr: TJSONArray;
  I, J, Failures: Integer;
  Name, Input: string;
  Got: TStringList;
  Match: Boolean;
begin
  if ParamCount >= 1 then
    CasesPath := ParamStr(1)
  else
    CasesPath := 'tests/conformance/drivers/chunker_conformance/cases.json';

  Data := GetJSON(ReadAll(CasesPath));
  try
    Root := Data as TJSONObject;
    CasesArr := Root.Arrays['cases'];
    Failures := 0;
    for I := 0 to CasesArr.Count - 1 do
    begin
      CaseObj := CasesArr.Objects[I];
      Name := CaseObj.Strings['name'];
      Input := CaseObj.Strings['input'];
      ExpectedArr := CaseObj.Arrays['expected'];
      Got := SplitStatements(Input);
      try
        Match := (Got.Count = ExpectedArr.Count);
        if Match then
          for J := 0 to ExpectedArr.Count - 1 do
            if Got[J] <> ExpectedArr.Strings[J] then
            begin
              Match := False;
              Break;
            end;
        if Match then
          WriteLn('[PASS] ', Name)
        else
        begin
          Inc(Failures);
          WriteLn('[FAIL] ', Name);
          WriteLn('   expected (', ExpectedArr.Count, '):');
          for J := 0 to ExpectedArr.Count - 1 do
            WriteLn('     | ', ExpectedArr.Strings[J]);
          WriteLn('   got (', Got.Count, '):');
          for J := 0 to Got.Count - 1 do
            WriteLn('     | ', Got[J]);
        end;
      finally
        Got.Free;
      end;
    end;
    WriteLn;
    WriteLn(CasesArr.Count - Failures, '/', CasesArr.Count,
      ' chunker conformance cases passed');
  finally
    Data.Free;
  end;
  if Failures > 0 then
    Halt(1);
end.
