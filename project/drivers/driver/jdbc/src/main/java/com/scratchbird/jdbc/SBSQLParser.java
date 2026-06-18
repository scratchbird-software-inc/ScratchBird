// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird JDBC Driver
 * Copyright (c) 2025 ScratchBird Project
 */
package com.scratchbird.jdbc;

import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * SQL parser utilities for JDBC escape sequences.
 */
public class SBSQLParser {

    /**
     * Converts JDBC escape sequences to native SQL.
     *
     * Supported escape sequences:
     * - {fn function_name(args)} - Scalar functions
     * - {d 'yyyy-mm-dd'} - Date literal
     * - {t 'hh:mm:ss'} - Time literal
     * - {ts 'yyyy-mm-dd hh:mm:ss'} - Timestamp literal
     * - {escape 'char'} - LIKE escape character
     * - {oj outer-join} - Outer join
     * - {call proc(args)} - Stored procedure call
     */
    public static String convertToNativeSQL(String sql) {
        if (sql == null || !sql.contains("{")) {
            return sql;
        }

        StringBuilder result = new StringBuilder();
        int i = 0;
        int len = sql.length();

        while (i < len) {
            char c = sql.charAt(i);

            if (c == '{') {
                // Find matching close brace
                int braceCount = 1;
                int start = i;
                i++;

                while (i < len && braceCount > 0) {
                    c = sql.charAt(i);
                    if (c == '{') braceCount++;
                    else if (c == '}') braceCount--;
                    i++;
                }

                if (braceCount == 0) {
                    String escape = sql.substring(start + 1, i - 1).trim();
                    result.append(processEscape(escape));
                } else {
                    // Unmatched brace, keep as-is
                    result.append(sql.substring(start));
                }
            } else {
                result.append(c);
                i++;
            }
        }

        return result.toString();
    }

    private static String processEscape(String escape) {
        if (escape.isEmpty()) {
            return "{}";
        }

        String lower = escape.toLowerCase();

        // Date literal: {d 'yyyy-mm-dd'}
        if (lower.startsWith("d ")) {
            String dateStr = escape.substring(2).trim();
            return "CAST(" + dateStr + " AS DATE)";
        }

        // Time literal: {t 'hh:mm:ss'}
        if (lower.startsWith("t ")) {
            String timeStr = escape.substring(2).trim();
            return "CAST(" + timeStr + " AS TIME)";
        }

        // Timestamp literal: {ts 'yyyy-mm-dd hh:mm:ss'}
        if (lower.startsWith("ts ")) {
            String tsStr = escape.substring(3).trim();
            return "CAST(" + tsStr + " AS TIMESTAMP)";
        }

        // Escape character: {escape 'char'}
        if (lower.startsWith("escape ")) {
            String escapeChar = escape.substring(7).trim();
            return "ESCAPE " + escapeChar;
        }

        // Outer join: {oj table1 LEFT OUTER JOIN table2 ON ...}
        if (lower.startsWith("oj ")) {
            return escape.substring(3).trim();
        }

        // Scalar function: {fn func(args)}
        if (lower.startsWith("fn ")) {
            String func = escape.substring(3).trim();
            return convertFunction(func);
        }

        // Stored procedure call: {call proc(args)}
        if (lower.startsWith("call ")) {
            String proc = escape.substring(5).trim();
            return "CALL " + proc;
        }

        // Stored function call with return placeholder: {? = call func(args)}
        if (lower.startsWith("?")) {
            int eq = escape.indexOf('=');
            if (eq > 0) {
                String rhs = escape.substring(eq + 1).trim();
                if (rhs.toLowerCase().startsWith("call ")) {
                    String fn = rhs.substring(5).trim();
                    return "SELECT " + fn;
                }
            }
        }

        // Unknown escape, return as-is
        return "{" + escape + "}";
    }

    private static String convertFunction(String func) {
        String lower = func.toLowerCase();

        // String functions
        if (lower.startsWith("concat(")) {
            return func;  // Native support
        }
        if (lower.startsWith("length(")) {
            return "LENGTH" + func.substring(6);
        }
        if (lower.startsWith("lcase(")) {
            return "LOWER" + func.substring(5);
        }
        if (lower.startsWith("ucase(")) {
            return "UPPER" + func.substring(5);
        }
        if (lower.startsWith("substring(")) {
            return "SUBSTRING" + func.substring(9);
        }
        if (lower.startsWith("ltrim(")) {
            return "LTRIM" + func.substring(5);
        }
        if (lower.startsWith("rtrim(")) {
            return "RTRIM" + func.substring(5);
        }

        // Numeric functions
        if (lower.startsWith("abs(")) {
            return "ABS" + func.substring(3);
        }
        if (lower.startsWith("mod(")) {
            return "MOD" + func.substring(3);
        }
        if (lower.startsWith("sqrt(")) {
            return "SQRT" + func.substring(4);
        }
        if (lower.startsWith("power(")) {
            return "POWER" + func.substring(5);
        }
        if (lower.startsWith("ceiling(")) {
            return "CEILING" + func.substring(7);
        }
        if (lower.startsWith("floor(")) {
            return "FLOOR" + func.substring(5);
        }
        if (lower.startsWith("round(")) {
            return "ROUND" + func.substring(5);
        }
        if (lower.startsWith("truncate(")) {
            return "TRUNC" + func.substring(8);
        }

        // Date/time functions
        if (lower.equals("curdate()")) {
            return "CURRENT_DATE";
        }
        if (lower.equals("curtime()")) {
            return "CURRENT_TIME";
        }
        if (lower.equals("now()")) {
            return "CURRENT_TIMESTAMP";
        }
        if (lower.startsWith("dayofmonth(")) {
            return "EXTRACT(DAY FROM " + func.substring(11, func.length() - 1) + ")";
        }
        if (lower.startsWith("month(")) {
            return "EXTRACT(MONTH FROM " + func.substring(6, func.length() - 1) + ")";
        }
        if (lower.startsWith("year(")) {
            return "EXTRACT(YEAR FROM " + func.substring(5, func.length() - 1) + ")";
        }
        if (lower.startsWith("hour(")) {
            return "EXTRACT(HOUR FROM " + func.substring(5, func.length() - 1) + ")";
        }
        if (lower.startsWith("minute(")) {
            return "EXTRACT(MINUTE FROM " + func.substring(7, func.length() - 1) + ")";
        }
        if (lower.startsWith("second(")) {
            return "EXTRACT(SECOND FROM " + func.substring(7, func.length() - 1) + ")";
        }

        // System functions
        if (lower.equals("database()")) {
            return "CURRENT_DATABASE()";
        }
        if (lower.equals("user()")) {
            return "CURRENT_USER";
        }

        // Default: return as-is
        return func;
    }

    /**
     * Matches a {@code SET TERM <terminator>} client directive (case-insensitive),
     * capturing the new terminator in group 1.
     */
    private static final Pattern SET_TERM_RE =
        Pattern.compile("^set\\s+term\\s+(\\S.*?)\\s*$", Pattern.CASE_INSENSITIVE);

    /**
     * Splits SQL into top-level statements on the active terminator.
     *
     * <p>Quote-aware (single/double quotes) and comment-aware ({@code --} line
     * comments ride along verbatim with the statement; {@code /} {@code *} ... {@code *}{@code /}
     * block comments are likewise copied verbatim). Honors the
     * {@code SET TERM <terminator>} client directive (Firebird / {@code sb_isql}
     * semantics): the directive changes the active terminator and is consumed — it
     * is not emitted as a statement and is not counted in statement indexing. This
     * lets procedural bodies (functions, procedures, triggers) contain inner
     * {@code ;} between {@code SET TERM ^} and the restoring {@code SET TERM ;^}.
     *
     * <p>With no {@code SET TERM} directive present, the behavior is identical to a
     * plain quote-aware top-level {@code ;} split, so existing scripts and statement
     * indices are unchanged. The chosen terminator must not appear in the bodies it
     * wraps.
     *
     * <p>This is the canonical cross-driver chunker; it is validated against
     * {@code tests/conformance/drivers/chunker_conformance/cases.json}.
     *
     * @param sql the SQL text to split (may be {@code null})
     * @return the list of trimmed top-level statements (never {@code null})
     */
    public static List<String> splitTopLevelStatements(String sql) {
        List<String> statements = new ArrayList<>();
        if (sql == null) {
            return statements;
        }

        StringBuilder current = new StringBuilder(sql.length());
        // term is a single-element holder so the flush helper can mutate it.
        String[] term = {";"};
        boolean inSingle = false;
        boolean inDouble = false;
        int i = 0;
        int length = sql.length();

        while (i < length) {
            char ch = sql.charAt(i);
            char next = (i + 1) < length ? sql.charAt(i + 1) : '\0';

            // `--` line comment: consume to end of line verbatim, without scanning
            // for the terminator or quotes inside it.
            if (!inSingle && !inDouble && ch == '-' && next == '-') {
                int eol = sql.indexOf('\n', i);
                if (eol == -1) {
                    eol = length;
                }
                current.append(sql, i, eol);
                i = eol;
                continue;
            }

            // `/* */` block comment: consume verbatim (not exercised by the
            // conformance fixture, but preserved for backward compatibility).
            if (!inSingle && !inDouble && ch == '/' && next == '*') {
                int close = sql.indexOf("*/", i + 2);
                if (close == -1) {
                    current.append(sql, i, length);
                    i = length;
                    continue;
                }
                current.append(sql, i, close + 2);
                i = close + 2;
                continue;
            }

            if (ch == '\'' && !inDouble) {
                inSingle = !inSingle;
                current.append(ch);
                i++;
                continue;
            }
            if (ch == '"' && !inSingle) {
                inDouble = !inDouble;
                current.append(ch);
                i++;
                continue;
            }

            if (!inSingle && !inDouble && !term[0].isEmpty() && sql.startsWith(term[0], i)) {
                int matchedLen = term[0].length(); // capture before flush() may change term
                flushChunk(statements, current, term);
                current.setLength(0);
                i += matchedLen;
                continue;
            }

            current.append(ch);
            i++;
        }

        flushChunk(statements, current, term);
        return statements;
    }

    private static void flushChunk(List<String> statements, StringBuilder buf, String[] term) {
        String chunk = buf.toString().trim();
        if (chunk.isEmpty()) {
            return;
        }
        String newTerm = chunkSetTerm(chunk);
        if (newTerm != null) {
            term[0] = newTerm;
            return;
        }
        statements.add(chunk);
    }

    /**
     * Returns the new terminator if {@code chunk} is a {@code SET TERM <terminator>}
     * client directive, else {@code null}.
     *
     * <p>Leading full-line {@code --} comments and blank lines are ignored when
     * matching, so a directive may be preceded by comment lines in the same chunk.
     */
    private static String chunkSetTerm(String chunk) {
        StringBuilder meaningful = new StringBuilder();
        for (String line : chunk.split("\n", -1)) {
            String stripped = line.strip();
            if (stripped.isEmpty() || stripped.startsWith("--")) {
                continue;
            }
            if (meaningful.length() > 0) {
                meaningful.append(' ');
            }
            meaningful.append(stripped);
        }
        if (meaningful.length() == 0) {
            return null;
        }
        Matcher match = SET_TERM_RE.matcher(meaningful.toString());
        if (match.matches()) {
            return match.group(1).strip();
        }
        return null;
    }
}
