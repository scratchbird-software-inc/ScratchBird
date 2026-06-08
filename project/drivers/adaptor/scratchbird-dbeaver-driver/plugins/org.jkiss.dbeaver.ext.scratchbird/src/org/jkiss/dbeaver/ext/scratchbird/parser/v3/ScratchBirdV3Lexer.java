// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * DBeaver - Universal Database Manager
 * Copyright (C) 2010-2026 DBeaver Corp and others
 *
 * Licensed under the Apache License, Version 2.0
 */
package org.jkiss.dbeaver.ext.scratchbird.parser.v3;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;

public final class ScratchBirdV3Lexer {
    private static final Map<String, ScratchBirdV3TokenType> GATEKEEPER_KEYWORDS = Map.ofEntries(
        Map.entry("SELECT", ScratchBirdV3TokenType.KW_SELECT),
        Map.entry("INSERT", ScratchBirdV3TokenType.KW_INSERT),
        Map.entry("UPDATE", ScratchBirdV3TokenType.KW_UPDATE),
        Map.entry("DELETE", ScratchBirdV3TokenType.KW_DELETE),
        Map.entry("MERGE", ScratchBirdV3TokenType.KW_MERGE),
        Map.entry("CREATE", ScratchBirdV3TokenType.KW_CREATE),
        Map.entry("ALTER", ScratchBirdV3TokenType.KW_ALTER),
        Map.entry("DROP", ScratchBirdV3TokenType.KW_DROP),
        Map.entry("TRUNCATE", ScratchBirdV3TokenType.KW_TRUNCATE),
        Map.entry("COPY", ScratchBirdV3TokenType.KW_COPY),
        Map.entry("GRANT", ScratchBirdV3TokenType.KW_GRANT),
        Map.entry("REVOKE", ScratchBirdV3TokenType.KW_REVOKE),
        Map.entry("COMMIT", ScratchBirdV3TokenType.KW_COMMIT),
        Map.entry("ROLLBACK", ScratchBirdV3TokenType.KW_ROLLBACK),
        Map.entry("BEGIN", ScratchBirdV3TokenType.KW_BEGIN),
        Map.entry("END", ScratchBirdV3TokenType.KW_END),
        Map.entry("DECLARE", ScratchBirdV3TokenType.KW_DECLARE),
        Map.entry("SET", ScratchBirdV3TokenType.KW_SET),
        Map.entry("SHOW", ScratchBirdV3TokenType.KW_SHOW),
        Map.entry("EXPLAIN", ScratchBirdV3TokenType.KW_EXPLAIN),
        Map.entry("ANALYZE", ScratchBirdV3TokenType.KW_ANALYZE),
        Map.entry("CALL", ScratchBirdV3TokenType.KW_CALL),
        Map.entry("EXECUTE", ScratchBirdV3TokenType.KW_EXECUTE),
        Map.entry("PREPARE", ScratchBirdV3TokenType.KW_PREPARE),
        Map.entry("FROM", ScratchBirdV3TokenType.KW_FROM),
        Map.entry("WHERE", ScratchBirdV3TokenType.KW_WHERE),
        Map.entry("GROUP", ScratchBirdV3TokenType.KW_GROUP),
        Map.entry("HAVING", ScratchBirdV3TokenType.KW_HAVING),
        Map.entry("ORDER", ScratchBirdV3TokenType.KW_ORDER),
        Map.entry("LIMIT", ScratchBirdV3TokenType.KW_LIMIT),
        Map.entry("OFFSET", ScratchBirdV3TokenType.KW_OFFSET),
        Map.entry("UNION", ScratchBirdV3TokenType.KW_UNION),
        Map.entry("INTERSECT", ScratchBirdV3TokenType.KW_INTERSECT),
        Map.entry("EXCEPT", ScratchBirdV3TokenType.KW_EXCEPT),
        Map.entry("WITH", ScratchBirdV3TokenType.KW_WITH),
        Map.entry("AND", ScratchBirdV3TokenType.KW_AND),
        Map.entry("OR", ScratchBirdV3TokenType.KW_OR),
        Map.entry("NOT", ScratchBirdV3TokenType.KW_NOT),
        Map.entry("IS", ScratchBirdV3TokenType.KW_IS),
        Map.entry("IN", ScratchBirdV3TokenType.KW_IN),
        Map.entry("BETWEEN", ScratchBirdV3TokenType.KW_BETWEEN),
        Map.entry("LIKE", ScratchBirdV3TokenType.KW_LIKE),
        Map.entry("DIV", ScratchBirdV3TokenType.KW_DIV),
        Map.entry("STARTING", ScratchBirdV3TokenType.KW_STARTING),
        Map.entry("CONTAINING", ScratchBirdV3TokenType.KW_CONTAINING),
        Map.entry("CASE", ScratchBirdV3TokenType.KW_CASE),
        Map.entry("WHEN", ScratchBirdV3TokenType.KW_WHEN),
        Map.entry("THEN", ScratchBirdV3TokenType.KW_THEN),
        Map.entry("ELSE", ScratchBirdV3TokenType.KW_ELSE),
        Map.entry("NULL", ScratchBirdV3TokenType.KW_NULL),
        Map.entry("TRUE", ScratchBirdV3TokenType.KW_TRUE),
        Map.entry("FALSE", ScratchBirdV3TokenType.KW_FALSE),
        Map.entry("EXISTS", ScratchBirdV3TokenType.KW_EXISTS),
        Map.entry("CAST", ScratchBirdV3TokenType.KW_CAST),
        Map.entry("AS", ScratchBirdV3TokenType.KW_AS),
        Map.entry("JOIN", ScratchBirdV3TokenType.KW_JOIN),
        Map.entry("ON", ScratchBirdV3TokenType.KW_ON),
        Map.entry("USING", ScratchBirdV3TokenType.KW_USING),
        Map.entry("LATERAL", ScratchBirdV3TokenType.KW_LATERAL),
        Map.entry("VALUES", ScratchBirdV3TokenType.KW_VALUES),
        Map.entry("INTO", ScratchBirdV3TokenType.KW_INTO),
        Map.entry("DEFAULT", ScratchBirdV3TokenType.KW_DEFAULT),
        Map.entry("START", ScratchBirdV3TokenType.KW_START),
        Map.entry("IF", ScratchBirdV3TokenType.KW_IF),
        Map.entry("RETURN", ScratchBirdV3TokenType.KW_RETURN)
    );

    private final String input;
    private final List<ScratchBirdV3Diagnostic> diagnostics = new ArrayList<>();
    private int offset;
    private int line = 1;
    private int column = 1;

    public ScratchBirdV3Lexer(String input) {
        this.input = input == null ? "" : input;
    }

    public List<ScratchBirdV3Diagnostic> diagnostics() {
        return diagnostics;
    }

    public List<ScratchBirdV3Token> tokenize() {
        List<ScratchBirdV3Token> tokens = new ArrayList<>();
        ScratchBirdV3Token token;
        do {
            token = nextToken();
            tokens.add(token);
        } while (token.type() != ScratchBirdV3TokenType.END_OF_FILE);
        return tokens;
    }

    private ScratchBirdV3Token nextToken() {
        skipTrivia();
        ScratchBirdV3SourceLocation start = location();
        if (isAtEnd()) {
            return token(ScratchBirdV3TokenType.END_OF_FILE, "", start, 0);
        }

        char c = peek();
        if ((c == 'x' || c == 'X') && peek(1) == '\'') {
            return blobLiteral(start);
        }
        if ((c == 'e' || c == 'E') && peek(1) == '\'') {
            advance();
            return stringLiteral(start, true);
        }
        if (c == '\'') {
            return stringLiteral(start, false);
        }
        if (c == '"') {
            return quotedIdentifier(start);
        }
        if (Character.isDigit(c)) {
            return number(start);
        }
        if (isIdentifierStart(c)) {
            return identifier(start);
        }
        if (c == '$' && Character.isDigit(peek(1))) {
            return positionalParameter(start);
        }
        if (c == ':' && isIdentifierStart(peek(1))) {
            return namedParameter(start);
        }

        return operatorOrPunctuation(start);
    }

    private void skipTrivia() {
        boolean again;
        do {
            again = false;
            while (!isAtEnd() && Character.isWhitespace(peek())) {
                advance();
            }
            if (peek() == '-' && peek(1) == '-') {
                while (!isAtEnd() && peek() != '\n') {
                    advance();
                }
                again = true;
            } else if (peek() == '/' && peek(1) == '*') {
                ScratchBirdV3SourceLocation start = location();
                advance();
                advance();
                int depth = 1;
                while (!isAtEnd() && depth > 0) {
                    if (peek() == '/' && peek(1) == '*') {
                        advance();
                        advance();
                        depth++;
                    } else if (peek() == '*' && peek(1) == '/') {
                        advance();
                        advance();
                        depth--;
                    } else {
                        advance();
                    }
                }
                if (depth > 0) {
                    diagnostics.add(error("PRS_JV3_001", "Unterminated block comment", "Close the comment with */", start, 2));
                }
                again = true;
            }
        } while (again);
    }

    private ScratchBirdV3Token identifier(ScratchBirdV3SourceLocation start) {
        int begin = offset;
        advance();
        while (!isAtEnd() && isIdentifierPart(peek())) {
            advance();
        }
        String text = input.substring(begin, offset);
        ScratchBirdV3TokenType type = GATEKEEPER_KEYWORDS.getOrDefault(text.toUpperCase(Locale.ROOT), ScratchBirdV3TokenType.IDENTIFIER);
        return token(type, text, start, offset - begin);
    }

    private ScratchBirdV3Token quotedIdentifier(ScratchBirdV3SourceLocation start) {
        int begin = offset;
        advance();
        while (!isAtEnd()) {
            if (peek() == '"') {
                advance();
                if (peek() == '"') {
                    advance();
                    continue;
                }
                return token(ScratchBirdV3TokenType.IDENTIFIER, input.substring(begin, offset), start, offset - begin);
            }
            advance();
        }
        diagnostics.add(error("PRS_JV3_002", "Unterminated quoted identifier", "Close the identifier with a double quote", start, offset - begin));
        return token(ScratchBirdV3TokenType.ERROR, input.substring(begin, offset), start, offset - begin);
    }

    private ScratchBirdV3Token stringLiteral(ScratchBirdV3SourceLocation start, boolean hasEscapePrefix) {
        int begin = hasEscapePrefix ? offset - 1 : offset;
        advance();
        while (!isAtEnd()) {
            if (peek() == '\'') {
                advance();
                if (peek() == '\'') {
                    advance();
                    continue;
                }
                return token(ScratchBirdV3TokenType.STRING_LITERAL, input.substring(begin, offset), start, offset - begin);
            }
            advance();
        }
        diagnostics.add(error("PRS_JV3_003", "Unterminated string literal", "Close the string literal with a single quote", start, offset - begin));
        return token(ScratchBirdV3TokenType.ERROR, input.substring(begin, offset), start, offset - begin);
    }

    private ScratchBirdV3Token blobLiteral(ScratchBirdV3SourceLocation start) {
        int begin = offset;
        advance();
        ScratchBirdV3Token literal = stringLiteral(start, false);
        return token(
            literal.type() == ScratchBirdV3TokenType.ERROR ? ScratchBirdV3TokenType.ERROR : ScratchBirdV3TokenType.BLOB_LITERAL,
            input.substring(begin, Math.min(offset, input.length())),
            start,
            offset - begin
        );
    }

    private ScratchBirdV3Token number(ScratchBirdV3SourceLocation start) {
        int begin = offset;
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
            advance();
            advance();
            while (isHexDigit(peek())) {
                advance();
            }
            return token(ScratchBirdV3TokenType.INTEGER_LITERAL, input.substring(begin, offset), start, offset - begin);
        }
        if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
            advance();
            advance();
            while (peek() == '0' || peek() == '1') {
                advance();
            }
            return token(ScratchBirdV3TokenType.INTEGER_LITERAL, input.substring(begin, offset), start, offset - begin);
        }
        while (Character.isDigit(peek())) {
            advance();
        }
        boolean floating = false;
        if (peek() == '.' && peek(1) != '.') {
            floating = true;
            advance();
            while (Character.isDigit(peek())) {
                advance();
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            floating = true;
            advance();
            if (peek() == '+' || peek() == '-') {
                advance();
            }
            while (Character.isDigit(peek())) {
                advance();
            }
        }
        return token(floating ? ScratchBirdV3TokenType.FLOAT_LITERAL : ScratchBirdV3TokenType.INTEGER_LITERAL,
            input.substring(begin, offset), start, offset - begin);
    }

    private ScratchBirdV3Token positionalParameter(ScratchBirdV3SourceLocation start) {
        int begin = offset;
        advance();
        while (Character.isDigit(peek())) {
            advance();
        }
        return token(ScratchBirdV3TokenType.PARAMETER, input.substring(begin, offset), start, offset - begin);
    }

    private ScratchBirdV3Token namedParameter(ScratchBirdV3SourceLocation start) {
        int begin = offset;
        advance();
        while (isIdentifierPart(peek())) {
            advance();
        }
        return token(ScratchBirdV3TokenType.PARAMETER, input.substring(begin, offset), start, offset - begin);
    }

    private ScratchBirdV3Token operatorOrPunctuation(ScratchBirdV3SourceLocation start) {
        int begin = offset;
        String three = "" + peek() + peek(1) + peek(2);
        if ("#>>".equals(three)) {
            advance(3);
            return token(ScratchBirdV3TokenType.HASH_DOUBLE_ARROW, input.substring(begin, offset), start, 3);
        }
        if ("-|-".equals(three)) {
            advance(3);
            return token(ScratchBirdV3TokenType.MINUS_PIPE_MINUS, input.substring(begin, offset), start, 3);
        }

        String two = "" + peek() + peek(1);
        ScratchBirdV3TokenType twoType = switch (two) {
            case "||" -> ScratchBirdV3TokenType.DOUBLE_PIPE;
            case "<>" -> ScratchBirdV3TokenType.NOT_EQUAL;
            case "!=" -> ScratchBirdV3TokenType.NOT_EQUAL;
            case "<=" -> ScratchBirdV3TokenType.LESS_EQUAL;
            case ">=" -> ScratchBirdV3TokenType.GREATER_EQUAL;
            case "<<" -> ScratchBirdV3TokenType.SHIFT_LEFT;
            case ">>" -> ScratchBirdV3TokenType.SHIFT_RIGHT;
            case "->" -> ScratchBirdV3TokenType.ARROW;
            case "#>" -> ScratchBirdV3TokenType.HASH_ARROW;
            case "?|" -> ScratchBirdV3TokenType.QUESTION_PIPE;
            case "?&" -> ScratchBirdV3TokenType.QUESTION_AMPERSAND;
            case "::" -> ScratchBirdV3TokenType.DOUBLE_COLON;
            case "@>" -> ScratchBirdV3TokenType.AT_GREATER;
            case "<@" -> ScratchBirdV3TokenType.LESS_AT;
            case "&&" -> ScratchBirdV3TokenType.DOUBLE_AMPERSAND;
            case "~*" -> ScratchBirdV3TokenType.TILDE_STAR;
            case "!~" -> ScratchBirdV3TokenType.EXCLAIM_TILDE;
            case ":=" -> ScratchBirdV3TokenType.COLON_EQUALS;
            case "=>" -> ScratchBirdV3TokenType.EQUALS_GREATER;
            case ".." -> ScratchBirdV3TokenType.DOUBLE_DOT;
            case "!:" -> ScratchBirdV3TokenType.EXCLAIM_COLON;
            default -> null;
        };
        if (twoType != null) {
            advance(2);
            if (twoType == ScratchBirdV3TokenType.EXCLAIM_TILDE && peek() == '*') {
                advance();
                return token(ScratchBirdV3TokenType.EXCLAIM_TILDE_STAR, input.substring(begin, offset), start, 3);
            }
            if (twoType == ScratchBirdV3TokenType.ARROW && peek() == '>') {
                advance();
                return token(ScratchBirdV3TokenType.DOUBLE_ARROW, input.substring(begin, offset), start, 3);
            }
            return token(twoType, input.substring(begin, offset), start, 2);
        }

        ScratchBirdV3TokenType oneType = switch (peek()) {
            case '+' -> ScratchBirdV3TokenType.PLUS;
            case '-' -> ScratchBirdV3TokenType.MINUS;
            case '*' -> ScratchBirdV3TokenType.STAR;
            case '/' -> ScratchBirdV3TokenType.SLASH;
            case '%' -> ScratchBirdV3TokenType.PERCENT;
            case '^' -> ScratchBirdV3TokenType.CARET;
            case '=' -> ScratchBirdV3TokenType.EQUAL;
            case '<' -> ScratchBirdV3TokenType.LESS_THAN;
            case '>' -> ScratchBirdV3TokenType.GREATER_THAN;
            case '&' -> ScratchBirdV3TokenType.AMPERSAND;
            case '|' -> ScratchBirdV3TokenType.PIPE;
            case '~' -> ScratchBirdV3TokenType.TILDE;
            case '@' -> ScratchBirdV3TokenType.AT_SIGN;
            case '?' -> ScratchBirdV3TokenType.QUESTION_MARK;
            case '(' -> ScratchBirdV3TokenType.LEFT_PAREN;
            case ')' -> ScratchBirdV3TokenType.RIGHT_PAREN;
            case '[' -> ScratchBirdV3TokenType.LEFT_BRACKET;
            case ']' -> ScratchBirdV3TokenType.RIGHT_BRACKET;
            case '{' -> ScratchBirdV3TokenType.LEFT_BRACE;
            case '}' -> ScratchBirdV3TokenType.RIGHT_BRACE;
            case ',' -> ScratchBirdV3TokenType.COMMA;
            case ';' -> ScratchBirdV3TokenType.SEMICOLON;
            case '.' -> ScratchBirdV3TokenType.DOT;
            case ':' -> ScratchBirdV3TokenType.COLON;
            default -> ScratchBirdV3TokenType.ERROR;
        };
        advance();
        if (oneType == ScratchBirdV3TokenType.ERROR) {
            diagnostics.add(error("PRS_JV3_004", "Unexpected character '" + input.charAt(begin) + "'", "", start, 1));
        }
        return token(oneType, input.substring(begin, offset), start, 1);
    }

    private ScratchBirdV3Diagnostic error(String code, String message, String hint, ScratchBirdV3SourceLocation start, int length) {
        return new ScratchBirdV3Diagnostic(
            ScratchBirdV3Diagnostic.Severity.ERROR,
            code,
            message,
            hint,
            new ScratchBirdV3SourceSpan(start, length)
        );
    }

    private ScratchBirdV3Token token(ScratchBirdV3TokenType type, String text, ScratchBirdV3SourceLocation start, int length) {
        return new ScratchBirdV3Token(type, text, new ScratchBirdV3SourceSpan(start, length));
    }

    private ScratchBirdV3SourceLocation location() {
        return new ScratchBirdV3SourceLocation(line, column, offset);
    }

    private boolean isAtEnd() {
        return offset >= input.length();
    }

    private char peek() {
        return peek(0);
    }

    private char peek(int lookahead) {
        int index = offset + lookahead;
        return index >= input.length() ? '\0' : input.charAt(index);
    }

    private void advance() {
        if (isAtEnd()) {
            return;
        }
        char c = input.charAt(offset++);
        if (c == '\n') {
            line++;
            column = 1;
        } else {
            column++;
        }
    }

    private void advance(int count) {
        for (int i = 0; i < count; i++) {
            advance();
        }
    }

    private static boolean isIdentifierStart(char c) {
        return c == '_' || Character.isLetter(c);
    }

    private static boolean isIdentifierPart(char c) {
        return c == '_' || c == '$' || Character.isLetterOrDigit(c);
    }

    private static boolean isHexDigit(char c) {
        return Character.digit(c, 16) >= 0;
    }
}
