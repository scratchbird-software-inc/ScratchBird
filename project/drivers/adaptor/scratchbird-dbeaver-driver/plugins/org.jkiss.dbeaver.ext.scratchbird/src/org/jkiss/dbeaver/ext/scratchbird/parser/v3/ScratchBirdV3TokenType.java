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

/**
 * Java mirror of the SBsql "smart parser, dumb lexer" gatekeeper token set.
 * Contextual SQL words intentionally remain IDENTIFIER tokens.
 */
public enum ScratchBirdV3TokenType {
    END_OF_FILE,
    ERROR,

    INTEGER_LITERAL,
    FLOAT_LITERAL,
    STRING_LITERAL,
    BLOB_LITERAL,
    IDENTIFIER,
    PARAMETER,

    PLUS,
    MINUS,
    STAR,
    SLASH,
    PERCENT,
    CARET,
    DOUBLE_PIPE,

    EQUAL,
    NOT_EQUAL,
    LESS_THAN,
    GREATER_THAN,
    LESS_EQUAL,
    GREATER_EQUAL,

    AMPERSAND,
    PIPE,
    TILDE,
    SHIFT_LEFT,
    SHIFT_RIGHT,

    ARROW,
    DOUBLE_ARROW,
    HASH_ARROW,
    HASH_DOUBLE_ARROW,
    AT_SIGN,
    QUESTION_MARK,
    QUESTION_PIPE,
    QUESTION_AMPERSAND,

    DOUBLE_COLON,
    AT_GREATER,
    LESS_AT,
    DOUBLE_AMPERSAND,
    MINUS_PIPE_MINUS,

    TILDE_STAR,
    EXCLAIM_TILDE,
    EXCLAIM_TILDE_STAR,

    COLON_EQUALS,
    EQUALS_GREATER,

    LEFT_PAREN,
    RIGHT_PAREN,
    LEFT_BRACKET,
    RIGHT_BRACKET,
    LEFT_BRACE,
    RIGHT_BRACE,
    COMMA,
    SEMICOLON,
    DOT,
    DOUBLE_DOT,
    EXCLAIM_COLON,
    COLON,

    KW_SELECT,
    KW_INSERT,
    KW_UPDATE,
    KW_DELETE,
    KW_MERGE,
    KW_CREATE,
    KW_ALTER,
    KW_DROP,
    KW_TRUNCATE,
    KW_COPY,
    KW_GRANT,
    KW_REVOKE,
    KW_COMMIT,
    KW_ROLLBACK,
    KW_BEGIN,
    KW_END,
    KW_DECLARE,
    KW_SET,
    KW_SHOW,
    KW_EXPLAIN,
    KW_ANALYZE,
    KW_CALL,
    KW_EXECUTE,
    KW_PREPARE,

    KW_FROM,
    KW_WHERE,
    KW_GROUP,
    KW_HAVING,
    KW_ORDER,
    KW_LIMIT,
    KW_OFFSET,
    KW_UNION,
    KW_INTERSECT,
    KW_EXCEPT,
    KW_WITH,

    KW_AND,
    KW_OR,
    KW_NOT,
    KW_IS,
    KW_IN,
    KW_BETWEEN,
    KW_LIKE,
    KW_DIV,
    KW_STARTING,
    KW_CONTAINING,
    KW_CASE,
    KW_WHEN,
    KW_THEN,
    KW_ELSE,
    KW_NULL,
    KW_TRUE,
    KW_FALSE,
    KW_EXISTS,
    KW_CAST,
    KW_AS,

    KW_JOIN,
    KW_ON,
    KW_USING,
    KW_LATERAL,

    KW_VALUES,
    KW_INTO,
    KW_DEFAULT,

    KW_START,
    KW_IF,
    KW_RETURN
}
