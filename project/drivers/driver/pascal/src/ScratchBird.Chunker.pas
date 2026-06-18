// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
//
// Canonical SET TERM- and comment-aware statement chunker shared by the
// Pascal driver tools (sb_isql_pascal) and the chunker conformance verifier.
// Keeping ONE implementation here (instead of a hand-maintained copy per tool)
// is what prevents the splitter divergence the chunker conformance fixture
// guards against.
//
// Behavior mirrors the C++ reference
// (drivers/driver/cpp/tools/sb_statement_chunker.hpp) and the Python reference
// (drivers/driver/python/src/scratchbird/sql.py::split_top_level_statements),
// and is verified against
// tests/conformance/drivers/chunker_conformance/cases.json.
unit ScratchBird.Chunker;

{$mode delphi}
{$H+}

interface

uses
  SysUtils, Classes;

// Split SQL into top-level statements on the active terminator.
//
// Quote-aware (single/double quotes) and `--` comment-aware. Honors the
// `SET TERM <terminator>` client directive (Firebird / sb_isql semantics):
// the directive changes the active terminator and is consumed (not emitted,
// not counted in statement indexing). With no SET TERM present, behavior is a
// plain quote-aware top-level `;` split, so existing scripts and indices stay
// stable. Caller owns the returned TStringList.
function SplitStatements(const Script: string): TStringList;

implementation

// Return the new terminator if `Chunk` is a `SET TERM <terminator>` client
// directive, else ''. Leading full-line `--` comments and blank lines are
// ignored when matching, so a directive may be preceded by comment lines.
// Mirrors sbchunk::setTermDirective.
function SetTermDirective(const Chunk: string): string;
var
  Lines: TStringList;
  Meaningful, T: string;
  I: Integer;
begin
  Result := '';
  Meaningful := '';
  Lines := TStringList.Create;
  try
    Lines.Text := Chunk;
    for I := 0 to Lines.Count - 1 do
    begin
      T := Trim(Lines[I]);
      if (T = '') or (Copy(T, 1, 2) = '--') then
        Continue;
      if Meaningful <> '' then
        Meaningful := Meaningful + ' ';
      Meaningful := Meaningful + T;
    end;
  finally
    Lines.Free;
  end;
  if Copy(LowerCase(Meaningful), 1, 8) <> 'set term' then
    Exit('');
  // Text after "set term"; '' if none.
  Result := Trim(Copy(Meaningful, 9, Length(Meaningful)));
end;

function SplitStatements(const Script: string): TStringList;
var
  Out: TStringList;
  Current: string;
  Term: string;
  Single, Dbl: Boolean;
  I, N, Eol, MatchedLen: Integer;
  Ch: Char;

  procedure Flush;
  var
    ChunkText, NewTerm: string;
  begin
    ChunkText := Trim(Current);
    if ChunkText = '' then
      Exit;
    NewTerm := SetTermDirective(ChunkText);
    if NewTerm <> '' then
    begin
      Term := NewTerm;
      Exit;
    end;
    Out.Add(ChunkText);
  end;

begin
  Out := TStringList.Create;
  Current := '';
  Term := ';';
  Single := False;
  Dbl := False;
  I := 1;
  N := Length(Script);
  while I <= N do
  begin
    Ch := Script[I];
    if (not Single) and (not Dbl) and (Ch = '-') and (I + 1 <= N) and
       (Script[I + 1] = '-') then
    begin
      // `--` line comment: copy to end of line verbatim, without scanning for
      // the terminator or quotes inside it. ';'/terminator chars in a comment
      // never split.
      Eol := I;
      while (Eol <= N) and (Script[Eol] <> #10) do
        Inc(Eol);
      Current := Current + Copy(Script, I, Eol - I);
      I := Eol;
      Continue;
    end;
    if (Ch = '''') and (not Dbl) then
    begin
      Single := not Single;
      Current := Current + Ch;
      Inc(I);
      Continue;
    end;
    if (Ch = '"') and (not Single) then
    begin
      Dbl := not Dbl;
      Current := Current + Ch;
      Inc(I);
      Continue;
    end;
    if (not Single) and (not Dbl) and (Term <> '') and
       (Copy(Script, I, Length(Term)) = Term) then
    begin
      MatchedLen := Length(Term);  // capture before Flush may change Term
      Flush;
      Current := '';
      I := I + MatchedLen;
      Continue;
    end;
    Current := Current + Ch;
    Inc(I);
  end;
  Flush;
  Result := Out;
end;

end.
