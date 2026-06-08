// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

export interface NormalizedQuery {
  sql: string;
  params: any[];
}

export interface PreparedQueryPlan {
  sql: string;
  paramCount: number;
  namedOrder: string[] | null;
}

interface CallableInvocation {
  routine: string;
  args: string;
  hasParens: boolean;
}

export function normalizeQuery(sql: string, params?: any[] | Record<string, any>): NormalizedQuery {
  if (!params) {
    return { sql, params: [] };
  }
  if (Array.isArray(params)) {
    if (sql.includes("?")) {
      const rewritten = rewritePositional(sql, params);
      return { sql: rewritten.sql, params: rewritten.params };
    }
    return { sql, params };
  }
  if (!hasNamedParams(sql)) {
    throw new Error("named parameters provided but query has no named placeholders");
  }
  const rewritten = rewriteNamed(sql, params);
  return { sql: rewritten.sql, params: rewritten.params };
}

export function normalizeCallableQuery(sql: string, params?: any[] | Record<string, any>): NormalizedQuery {
  const callableSql = normalizeCallableSql(sql);
  return normalizeQuery(callableSql, params);
}

export function normalizePreparedQuery(sql: string): PreparedQueryPlan {
  if (hasNamedParams(sql)) {
    const rewritten = rewriteNamedForPrepare(sql);
    return {
      sql: rewritten.sql,
      paramCount: rewritten.namedOrder.length,
      namedOrder: rewritten.namedOrder,
    };
  }
  const rewritten = rewritePositionalForPrepare(sql);
  return {
    sql: rewritten.sql,
    paramCount: rewritten.paramCount,
    namedOrder: null,
  };
}

export function normalizeCallableSql(sql: string): string {
  const trimmed = sql.trim();
  if (!(trimmed.startsWith("{") && trimmed.endsWith("}"))) {
    return sql;
  }
  const inner = trimmed.slice(1, -1).trim();
  if (!inner) {
    return sql;
  }

  const functionMatch = /^\?\s*=\s*call\s+([\s\S]+)$/i.exec(inner);
  if (functionMatch) {
    const invocation = parseCallableInvocation(functionMatch[1].trim());
    if (!invocation) {
      throw new Error("invalid JDBC escape call syntax");
    }
    const args = invocation.hasParens ? invocation.args : "";
    return `select ${invocation.routine}(${args}) as return_value`;
  }

  const procedureMatch = /^call\s+([\s\S]+)$/i.exec(inner);
  if (procedureMatch) {
    const invocation = parseCallableInvocation(procedureMatch[1].trim());
    if (!invocation) {
      throw new Error("invalid JDBC escape call syntax");
    }
    if (invocation.hasParens) {
      return `call ${invocation.routine}(${invocation.args})`;
    }
    return `call ${invocation.routine}`;
  }

  return sql;
}

export function splitTopLevelStatements(sql: string): string[] {
  const statements: string[] = [];
  let current = "";
  let inSingle = false;
  let inDouble = false;
  for (let i = 0; i < sql.length; i++) {
    const ch = sql[i];
    if (ch === "'" && !inDouble) {
      inSingle = !inSingle;
      current += ch;
      continue;
    }
    if (ch === '"' && !inSingle) {
      inDouble = !inDouble;
      current += ch;
      continue;
    }
    if (!inSingle && !inDouble && ch === ";") {
      const trimmed = current.trim();
      if (trimmed) {
        statements.push(trimmed);
      }
      current = "";
      continue;
    }
    current += ch;
  }
  const trimmed = current.trim();
  if (trimmed) {
    statements.push(trimmed);
  }
  return statements;
}

function hasNamedParams(sql: string): boolean {
  let inString = false;
  for (let i = 0; i + 1 < sql.length; i++) {
    const ch = sql[i];
    if (ch === "'") {
      inString = !inString;
      continue;
    }
    if (inString) continue;
    if ((ch === ":" || ch === "@") && isIdentStart(sql[i + 1])) {
      return true;
    }
  }
  return false;
}

function rewriteNamed(sql: string, params: Record<string, any>): NormalizedQuery {
  const lookup: Record<string, any> = {};
  for (const [key, value] of Object.entries(params)) {
    lookup[key.replace(/^[@:]/, "")] = value;
  }
  let result = "";
  const ordered: any[] = [];
  let inString = false;
  for (let i = 0; i < sql.length; ) {
    const ch = sql[i];
    if (ch === "'") {
      inString = !inString;
      result += ch;
      i++;
      continue;
    }
    if (!inString && (ch === ":" || ch === "@") && i + 1 < sql.length && isIdentStart(sql[i + 1])) {
      let j = i + 1;
      while (j < sql.length && isIdentPart(sql[j])) j++;
      const key = sql.slice(i + 1, j);
      if (!(key in lookup)) {
        throw new Error(`missing named parameter: ${key}`);
      }
      ordered.push(lookup[key]);
      result += `$${ordered.length}`;
      i = j;
      continue;
    }
    result += ch;
    i++;
  }
  return { sql: result, params: ordered };
}

function rewritePositional(sql: string, params: any[]): NormalizedQuery {
  let result = "";
  const ordered: any[] = [];
  let inString = false;
  let index = 0;
  for (let i = 0; i < sql.length; ) {
    const ch = sql[i];
    if (ch === "'") {
      inString = !inString;
      result += ch;
      i++;
      continue;
    }
    if (!inString && ch === "?") {
      if (index >= params.length) {
        throw new Error("not enough parameters");
      }
      ordered.push(params[index]);
      index++;
      result += `$${ordered.length}`;
      i++;
      continue;
    }
    result += ch;
    i++;
  }
  if (index < params.length) {
    throw new Error("too many parameters");
  }
  return { sql: result, params: ordered };
}

function rewriteNamedForPrepare(sql: string): { sql: string; namedOrder: string[] } {
  let result = "";
  const namedOrder: string[] = [];
  let inString = false;
  for (let i = 0; i < sql.length; ) {
    const ch = sql[i];
    if (ch === "'") {
      inString = !inString;
      result += ch;
      i++;
      continue;
    }
    if (!inString && (ch === ":" || ch === "@") && i + 1 < sql.length && isIdentStart(sql[i + 1])) {
      let j = i + 1;
      while (j < sql.length && isIdentPart(sql[j])) j++;
      namedOrder.push(sql.slice(i + 1, j));
      result += `$${namedOrder.length}`;
      i = j;
      continue;
    }
    result += ch;
    i++;
  }
  return { sql: result, namedOrder };
}

function rewritePositionalForPrepare(sql: string): { sql: string; paramCount: number } {
  let result = "";
  let inString = false;
  let paramCount = 0;
  for (let i = 0; i < sql.length; ) {
    const ch = sql[i];
    if (ch === "'") {
      inString = !inString;
      result += ch;
      i++;
      continue;
    }
    if (!inString && ch === "?") {
      paramCount++;
      result += `$${paramCount}`;
      i++;
      continue;
    }
    result += ch;
    i++;
  }
  return { sql: result, paramCount };
}

function parseCallableInvocation(text: string): CallableInvocation | null {
  const openParen = text.indexOf("(");
  if (openParen < 0) {
    const routine = text.trim();
    if (!routine) return null;
    return { routine, args: "", hasParens: false };
  }
  let inSingle = false;
  let inDouble = false;
  let depth = 0;
  let closeParen = -1;
  for (let i = openParen; i < text.length; i++) {
    const ch = text[i];
    if (ch === "'" && !inDouble) {
      inSingle = !inSingle;
      continue;
    }
    if (ch === '"' && !inSingle) {
      inDouble = !inDouble;
      continue;
    }
    if (inSingle || inDouble) {
      continue;
    }
    if (ch === "(") {
      depth++;
      continue;
    }
    if (ch === ")") {
      depth--;
      if (depth === 0) {
        closeParen = i;
        break;
      }
    }
  }
  if (closeParen < 0) {
    return null;
  }
  const routine = text.slice(0, openParen).trim();
  if (!routine) {
    return null;
  }
  const trailing = text.slice(closeParen + 1).trim();
  if (trailing) {
    return null;
  }
  const args = text.slice(openParen + 1, closeParen).trim();
  return { routine, args, hasParens: true };
}

function isIdentStart(ch: string): boolean {
  return /[A-Za-z_]/.test(ch);
}

function isIdentPart(ch: string): boolean {
  return /[A-Za-z0-9_]/.test(ch);
}
