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

import org.jkiss.code.NotNull;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;
import java.util.Set;

public final class ScratchBirdV3Parser {
    private final String sql;
    private final List<ScratchBirdV3Diagnostic> diagnostics = new ArrayList<>();

    public ScratchBirdV3Parser(@NotNull String sql) {
        this.sql = sql;
    }

    @NotNull
    public static ScratchBirdV3ParseResult parse(@NotNull String sql) {
        return new ScratchBirdV3Parser(sql).parseStatements();
    }

    @NotNull
    public ScratchBirdV3ParseResult parseStatements() {
        ScratchBirdV3Lexer lexer = new ScratchBirdV3Lexer(sql);
        List<ScratchBirdV3Token> tokens = lexer.tokenize();
        diagnostics.addAll(lexer.diagnostics());

        List<ScratchBirdV3Statement> statements = new ArrayList<>();
        List<ScratchBirdV3Token> current = new ArrayList<>();
        int parenDepth = 0;
        int bracketDepth = 0;
        int braceDepth = 0;
        int beginEndDepth = 0;

        for (ScratchBirdV3Token token : tokens) {
            if (token.type() == ScratchBirdV3TokenType.END_OF_FILE) {
                break;
            }
            if (token.type() == ScratchBirdV3TokenType.SEMICOLON &&
                parenDepth == 0 &&
                bracketDepth == 0 &&
                braceDepth == 0 &&
                beginEndDepth <= 0) {
                if (!current.isEmpty()) {
                    statements.add(parseStatement(current, true));
                    current = new ArrayList<>();
                }
                continue;
            }

            current.add(token);
            switch (token.type()) {
                case LEFT_PAREN -> parenDepth++;
                case RIGHT_PAREN -> parenDepth = Math.max(0, parenDepth - 1);
                case LEFT_BRACKET -> bracketDepth++;
                case RIGHT_BRACKET -> bracketDepth = Math.max(0, bracketDepth - 1);
                case LEFT_BRACE -> braceDepth++;
                case RIGHT_BRACE -> braceDepth = Math.max(0, braceDepth - 1);
                case KW_BEGIN -> {
                    if (current.size() > 1) {
                        beginEndDepth++;
                    }
                }
                case KW_END -> {
                    if (beginEndDepth > 0) {
                        beginEndDepth--;
                    }
                }
                default -> {
                }
            }
        }

        if (!current.isEmpty()) {
            statements.add(parseStatement(current, false));
        }

        if (statements.isEmpty() && diagnostics.isEmpty() && !sql.isBlank()) {
            diagnostics.add(error("PRS_JV3_100", "Expected SQL statement", "", spanForOffset(0)));
        }
        return new ScratchBirdV3ParseResult(List.copyOf(statements), List.copyOf(diagnostics));
    }

    @NotNull
    public static List<ScratchBirdV3Completion> completionsAt(@NotNull String sql, int offset) {
        String prefix = sql.substring(0, Math.max(0, Math.min(offset, sql.length())));
        ScratchBirdV3Lexer lexer = new ScratchBirdV3Lexer(prefix);
        List<ScratchBirdV3Token> tokens = lexer.tokenize().stream()
            .filter(token -> token.type() != ScratchBirdV3TokenType.END_OF_FILE)
            .toList();
        int lastSeparator = -1;
        for (int i = 0; i < tokens.size(); i++) {
            if (tokens.get(i).type() == ScratchBirdV3TokenType.SEMICOLON) {
                lastSeparator = i;
            }
        }
        List<ScratchBirdV3Token> statementTokens = tokens.subList(lastSeparator + 1, tokens.size()).stream()
            .filter(token -> token.type() != ScratchBirdV3TokenType.SEMICOLON)
            .toList();

        String activePrefix = "";
        List<ScratchBirdV3Token> contextTokens = statementTokens;
        if (!statementTokens.isEmpty()) {
            ScratchBirdV3Token last = statementTokens.get(statementTokens.size() - 1);
            if (last.isIdentifierLike() && last.span().endOffset() == prefix.length()) {
                activePrefix = upper(last);
                contextTokens = statementTokens.subList(0, statementTokens.size() - 1);
            }
        }

        if (contextTokens.isEmpty()) {
            return filterCompletions(ScratchBirdV3StatementCatalog.statementCompletions(), activePrefix);
        }
        String first = upper(contextTokens.get(0));
        if ("CREATE".equals(first) || "RECREATE".equals(first)) {
            return createCompletions(contextTokens, activePrefix);
        }
        if ("ALTER".equals(first)) {
            return objectCompletions(ScratchBirdV3StatementCatalog.ALTER_OBJECTS, "ALTER object type", activePrefix);
        }
        if ("DROP".equals(first)) {
            return objectCompletions(ScratchBirdV3StatementCatalog.DROP_OBJECTS, "DROP object type", activePrefix);
        }
        if ("SHOW".equals(first)) {
            return showCompletions(contextTokens, activePrefix);
        }
        if ("SET".equals(first)) {
            return setCompletions(contextTokens, activePrefix);
        }
        if ("RESET".equals(first)) {
            return objectCompletions(ScratchBirdV3StatementCatalog.RESET_PREFIXES, "RESET surface", activePrefix);
        }
        if ("EXECUTE".equals(first)) {
            return objectCompletions(ScratchBirdV3StatementCatalog.EXECUTE_PREFIXES, "EXECUTE surface", activePrefix);
        }
        if ("DESCRIBE".equals(first)) {
            return objectCompletions(ScratchBirdV3StatementCatalog.DESCRIBE_OBJECTS, "DESCRIBE object type", activePrefix);
        }
        if ("USE".equals(first)) {
            return objectCompletions(ScratchBirdV3StatementCatalog.USE_PREFIXES, "USE surface", activePrefix);
        }
        return filterCompletions(ScratchBirdV3StatementCatalog.statementCompletions(), activePrefix);
    }

    private ScratchBirdV3Statement parseStatement(List<ScratchBirdV3Token> tokens, boolean complete) {
        validateDelimiterBalance(tokens);
        ScratchBirdV3Token first = tokens.get(0);
        ScratchBirdV3SourceSpan span = span(tokens);

        if (first.type() == ScratchBirdV3TokenType.LEFT_BRACE) {
            diagnostics.add(error(
                "PRS_0505",
                "JDBC escape blocks are not supported in ScratchBird v3",
                "Use canonical SQL forms instead of {fn ...}, {d ...}, or {ts ...}",
                first.span()
            ));
            return statement(ScratchBirdV3StatementKind.UNKNOWN, ScratchBirdV3StatementFamily.UNKNOWN, "JDBC_ESCAPE", tokens, span, complete);
        }

        String keyword = upper(first);
        if (ScratchBirdV3StatementCatalog.REMOVED_ALIASES.contains(keyword)) {
            diagnostics.add(removedAliasDiagnostic(first));
            return statement(ScratchBirdV3StatementKind.UNKNOWN, ScratchBirdV3StatementFamily.UNKNOWN, keyword, tokens, span, complete);
        }

        return switch (first.type()) {
            case KW_WITH -> statement(ScratchBirdV3StatementKind.WITH, ScratchBirdV3StatementFamily.DML, "WITH", tokens, span, complete);
            case KW_CREATE -> parseCreate(tokens, span, complete);
            case KW_ALTER -> parseAlter(tokens, span, complete);
            case KW_DROP -> parseDrop(tokens, span, complete);
            case KW_TRUNCATE -> statement(ScratchBirdV3StatementKind.TRUNCATE, ScratchBirdV3StatementFamily.DDL, "TRUNCATE", tokens, span, complete);
            case KW_DECLARE -> parseDeclare(tokens, span, complete);
            case KW_SELECT -> statement(ScratchBirdV3StatementKind.SELECT, ScratchBirdV3StatementFamily.DML, "SELECT", tokens, span, complete);
            case KW_INSERT -> statement(ScratchBirdV3StatementKind.INSERT, ScratchBirdV3StatementFamily.DML, "INSERT", tokens, span, complete);
            case KW_UPDATE -> parseUpdate(tokens, span, complete);
            case KW_DELETE -> statement(ScratchBirdV3StatementKind.DELETE, ScratchBirdV3StatementFamily.DML, "DELETE", tokens, span, complete);
            case KW_COPY -> statement(ScratchBirdV3StatementKind.COPY, ScratchBirdV3StatementFamily.DML, "COPY", tokens, span, complete);
            case KW_MERGE -> statement(ScratchBirdV3StatementKind.MERGE, ScratchBirdV3StatementFamily.DML, "MERGE", tokens, span, complete);
            case KW_BEGIN -> statement(ScratchBirdV3StatementKind.BEGIN, ScratchBirdV3StatementFamily.TRANSACTION, "BEGIN", tokens, span, complete);
            case KW_START -> parseStart(tokens, span, complete);
            case KW_PREPARE -> statement(ScratchBirdV3StatementKind.PREPARE, ScratchBirdV3StatementFamily.TRANSACTION, "PREPARE", tokens, span, complete);
            case KW_COMMIT -> statement(ScratchBirdV3StatementKind.COMMIT, ScratchBirdV3StatementFamily.TRANSACTION, "COMMIT", tokens, span, complete);
            case KW_ROLLBACK -> statement(ScratchBirdV3StatementKind.ROLLBACK, ScratchBirdV3StatementFamily.TRANSACTION, "ROLLBACK", tokens, span, complete);
            case KW_SET -> parseSet(tokens, span, complete);
            case KW_SHOW -> parseShow(tokens, span, complete);
            case KW_EXPLAIN -> statement(ScratchBirdV3StatementKind.EXPLAIN, ScratchBirdV3StatementFamily.UTILITY, "EXPLAIN", tokens, span, complete);
            case KW_ANALYZE -> statement(ScratchBirdV3StatementKind.ANALYZE, ScratchBirdV3StatementFamily.UTILITY, "ANALYZE", tokens, span, complete);
            case KW_EXECUTE -> parseExecute(tokens, span, complete);
            case KW_CALL -> statement(ScratchBirdV3StatementKind.CALL, ScratchBirdV3StatementFamily.PSQL, "CALL", tokens, span, complete);
            case KW_GRANT -> statement(ScratchBirdV3StatementKind.GRANT, ScratchBirdV3StatementFamily.DCL, "GRANT", tokens, span, complete);
            case KW_REVOKE -> statement(ScratchBirdV3StatementKind.REVOKE, ScratchBirdV3StatementFamily.DCL, "REVOKE", tokens, span, complete);
            case KW_IF -> statement(ScratchBirdV3StatementKind.PSQL_IF, ScratchBirdV3StatementFamily.PSQL, "IF", tokens, span, complete);
            case KW_RETURN -> statement(ScratchBirdV3StatementKind.PSQL_RETURN, ScratchBirdV3StatementFamily.PSQL, "RETURN", tokens, span, complete);
            default -> parseContextual(tokens, span, complete);
        };
    }

    private ScratchBirdV3Statement parseContextual(List<ScratchBirdV3Token> tokens, ScratchBirdV3SourceSpan span, boolean complete) {
        String first = upper(tokens.get(0));
        if ("REPLACE".equals(first)) {
            diagnostics.add(error(
                "PRS_JV3_101",
                "REPLACE is MySQL compatibility syntax and is feature-gated in SBsql",
                "Use INSERT ... ON CONFLICT for canonical ScratchBird SQL",
                tokens.get(0).span()
            ));
            return statement(ScratchBirdV3StatementKind.INSERT, ScratchBirdV3StatementFamily.DML, "REPLACE", tokens, span, complete);
        }
        ScratchBirdV3StatementKind kind = ScratchBirdV3StatementCatalog.TOP_LEVEL_CONTEXTUAL.get(first);
        if (kind == null) {
            diagnostics.add(error("PRS_JV3_102", "Expected SQL statement", "", tokens.get(0).span()));
            return statement(ScratchBirdV3StatementKind.UNKNOWN, ScratchBirdV3StatementFamily.UNKNOWN, first, tokens, span, complete);
        }
        if (kind == ScratchBirdV3StatementKind.DESCRIBE) {
            return statement(kind, ScratchBirdV3StatementFamily.SESSION, "DESCRIBE", tokens, span, complete);
        }
        if (kind == ScratchBirdV3StatementKind.USE ||
            kind == ScratchBirdV3StatementKind.RESET ||
            kind == ScratchBirdV3StatementKind.CONFIG) {
            return statement(kind, ScratchBirdV3StatementFamily.SESSION, first, tokens, span, complete);
        }
        if (kind == ScratchBirdV3StatementKind.CONNECT || kind == ScratchBirdV3StatementKind.DISCONNECT) {
            return statement(kind, ScratchBirdV3StatementFamily.CONNECTION, first, tokens, span, complete);
        }
        if (kind == ScratchBirdV3StatementKind.COMMENT || kind == ScratchBirdV3StatementKind.SECURITY_LABEL) {
            return statement(kind, ScratchBirdV3StatementFamily.METADATA, surface(tokens, 2), tokens, span, complete);
        }
        if (kind == ScratchBirdV3StatementKind.SAVEPOINT) {
            return statement(kind, ScratchBirdV3StatementFamily.TRANSACTION, first, tokens, span, complete);
        }
        ScratchBirdV3StatementFamily family = switch (kind) {
            case DOC_PATH_FILTER, TIME_BUCKET_AGG, SEARCH_DSL, VECTOR_ANN, GRAPH_PATH, REDIS, HYBRID_BRIDGE -> ScratchBirdV3StatementFamily.NOSQL;
            case VALIDATE, SWEEP, CANCEL_JOB, ANALYZE, EXPLAIN -> ScratchBirdV3StatementFamily.UTILITY;
            default -> ScratchBirdV3StatementFamily.MANAGEMENT;
        };
        return statement(kind, family, surface(tokens, 3), tokens, span, complete);
    }

    private ScratchBirdV3Statement parseCreate(List<ScratchBirdV3Token> tokens, ScratchBirdV3SourceSpan span, boolean complete) {
        ObjectMatch match = matchDdlObject(tokens, 1, ScratchBirdV3StatementCatalog.CREATE_OBJECTS);
        if (match.surface == null) {
            addExpectedObjectDiagnostic("CREATE", tokens);
            return statement(ScratchBirdV3StatementKind.CREATE, ScratchBirdV3StatementFamily.DDL, "CREATE", tokens, span, complete);
        }
        if ("SEARCH".equals(match.surface) || "VECTOR".equals(match.surface)) {
            diagnostics.add(error("PRS_0505", "CREATE " + match.surface + " INDEX is not supported in v3", "Use CREATE INDEX ... USING <method>", tokens.get(1).span()));
        }
        if ("MEASUREMENT".equals(match.surface) || "SCHEDULE".equals(match.surface)) {
            diagnostics.add(error("PRS_0505", "Top-level CREATE " + match.surface + " is not supported in v3", "Use CREATE JOB ...", tokens.get(1).span()));
        }
        return statement(ScratchBirdV3StatementKind.CREATE, ScratchBirdV3StatementFamily.DDL, "CREATE " + match.surface, tokens, span, complete);
    }

    private ScratchBirdV3Statement parseAlter(List<ScratchBirdV3Token> tokens, ScratchBirdV3SourceSpan span, boolean complete) {
        ObjectMatch match = matchDdlObject(tokens, 1, ScratchBirdV3StatementCatalog.ALTER_OBJECTS);
        if (match.surface == null) {
            addExpectedObjectDiagnostic("ALTER", tokens);
            return statement(ScratchBirdV3StatementKind.ALTER, ScratchBirdV3StatementFamily.DDL, "ALTER", tokens, span, complete);
        }
        return statement(ScratchBirdV3StatementKind.ALTER, ScratchBirdV3StatementFamily.DDL, "ALTER " + match.surface, tokens, span, complete);
    }

    private ScratchBirdV3Statement parseDrop(List<ScratchBirdV3Token> tokens, ScratchBirdV3SourceSpan span, boolean complete) {
        ObjectMatch match = matchDdlObject(tokens, 1, ScratchBirdV3StatementCatalog.DROP_OBJECTS);
        if (match.surface == null) {
            addExpectedObjectDiagnostic("DROP", tokens);
            return statement(ScratchBirdV3StatementKind.DROP, ScratchBirdV3StatementFamily.DDL, "DROP", tokens, span, complete);
        }
        if ("MATERIALIZED".equals(match.surface)) {
            diagnostics.add(error("PRS_0505", "DROP MATERIALIZED VIEW is not supported in v3", "Use DROP VIEW", tokens.get(1).span()));
        }
        return statement(ScratchBirdV3StatementKind.DROP, ScratchBirdV3StatementFamily.DDL, "DROP " + match.surface, tokens, span, complete);
    }

    private ScratchBirdV3Statement parseDeclare(List<ScratchBirdV3Token> tokens, ScratchBirdV3SourceSpan span, boolean complete) {
        if (hasContext(tokens, 1, "CURSOR")) {
            return statement(ScratchBirdV3StatementKind.DECLARE, ScratchBirdV3StatementFamily.PSQL, "DECLARE CURSOR", tokens, span, complete);
        }
        if (hasContext(tokens, 1, "EXTERNAL") || hasContext(tokens, 1, "VARIABLE")) {
            return statement(ScratchBirdV3StatementKind.DECLARE, ScratchBirdV3StatementFamily.PSQL, surface(tokens, 3), tokens, span, complete);
        }
        return statement(ScratchBirdV3StatementKind.DECLARE, ScratchBirdV3StatementFamily.PSQL, "DECLARE", tokens, span, complete);
    }

    private ScratchBirdV3Statement parseUpdate(List<ScratchBirdV3Token> tokens, ScratchBirdV3SourceSpan span, boolean complete) {
        if (hasContext(tokens, 1, "OR") && hasContext(tokens, 2, "INSERT")) {
            return statement(ScratchBirdV3StatementKind.UPDATE_OR_INSERT, ScratchBirdV3StatementFamily.DML, "UPDATE OR INSERT", tokens, span, complete);
        }
        return statement(ScratchBirdV3StatementKind.UPDATE, ScratchBirdV3StatementFamily.DML, "UPDATE", tokens, span, complete);
    }

    private ScratchBirdV3Statement parseStart(List<ScratchBirdV3Token> tokens, ScratchBirdV3SourceSpan span, boolean complete) {
        if (hasContext(tokens, 1, "MIGRATION")) {
            return statement(ScratchBirdV3StatementKind.MIGRATION_CONTROL, ScratchBirdV3StatementFamily.MANAGEMENT, surface(tokens, 3), tokens, span, complete);
        }
        if (hasAnyContext(tokens, 1, Set.of("MANAGER", "LISTENER", "PARSER"))) {
            return statement(ScratchBirdV3StatementKind.MANAGEMENT_CONTROL, ScratchBirdV3StatementFamily.MANAGEMENT, surface(tokens, 3), tokens, span, complete);
        }
        return statement(ScratchBirdV3StatementKind.START, ScratchBirdV3StatementFamily.TRANSACTION, "START", tokens, span, complete);
    }

    private ScratchBirdV3Statement parseSet(List<ScratchBirdV3Token> tokens, ScratchBirdV3SourceSpan span, boolean complete) {
        if (tokens.size() == 1) {
            diagnostics.add(error("PRS_0504", "Expected SET target", "", tokens.get(0).span()));
        } else if (hasContext(tokens, 1, "PARSER") && hasContext(tokens, 2, "VERSION")) {
            diagnostics.add(error("PRS_0505", "SET PARSER VERSION is not supported", "Parser version is selected by the server and parser lane", tokens.get(1).span()));
        } else if ((hasContext(tokens, 1, "SCHEMA") || hasContext(tokens, 1, "CURRENT_SCHEMA")) && tokens.size() < 3) {
            diagnostics.add(error("PRS_0504", "Expected schema path or DEFAULT after SET SCHEMA", "", tokens.get(1).span()));
        }
        return statement(ScratchBirdV3StatementKind.SET, ScratchBirdV3StatementFamily.SESSION, surface(tokens, 3), tokens, span, complete);
    }

    private ScratchBirdV3Statement parseShow(List<ScratchBirdV3Token> tokens, ScratchBirdV3SourceSpan span, boolean complete) {
        if (tokens.size() == 1) {
            diagnostics.add(error("PRS_0504", "Expected variable name or SHOW keyword", "", tokens.get(0).span()));
            return statement(ScratchBirdV3StatementKind.SHOW, ScratchBirdV3StatementFamily.SESSION, "SHOW", tokens, span, complete);
        }
        String second = upper(tokens.get(1));
        if (ScratchBirdV3StatementCatalog.SHOW_CONTROL_PREFIXES.contains(second)) {
            ScratchBirdV3StatementKind kind = switch (second) {
                case "CLUSTER" -> ScratchBirdV3StatementKind.CLUSTER_CONTROL;
                case "CUBE" -> ScratchBirdV3StatementKind.CUBE_CONTROL;
                default -> ScratchBirdV3StatementKind.MANAGEMENT_CONTROL;
            };
            return statement(kind, ScratchBirdV3StatementFamily.MANAGEMENT, surface(tokens, 4), tokens, span, complete);
        }
        return statement(ScratchBirdV3StatementKind.SHOW, ScratchBirdV3StatementFamily.SESSION, surface(tokens, 4), tokens, span, complete);
    }

    private ScratchBirdV3Statement parseExecute(List<ScratchBirdV3Token> tokens, ScratchBirdV3SourceSpan span, boolean complete) {
        if (hasContext(tokens, 1, "BLOCK")) {
            return statement(ScratchBirdV3StatementKind.EXECUTE_BLOCK, ScratchBirdV3StatementFamily.PSQL, "EXECUTE BLOCK", tokens, span, complete);
        }
        if (hasContext(tokens, 1, "JOB")) {
            return statement(ScratchBirdV3StatementKind.EXECUTE_JOB, ScratchBirdV3StatementFamily.MANAGEMENT, "EXECUTE JOB", tokens, span, complete);
        }
        return statement(ScratchBirdV3StatementKind.EXECUTE, ScratchBirdV3StatementFamily.PSQL, surface(tokens, 3), tokens, span, complete);
    }

    private ObjectMatch matchDdlObject(List<ScratchBirdV3Token> tokens, int startIndex, Set<String> allowedObjects) {
        int index = skipCreateModifiers(tokens, startIndex);
        return bestSurfaceMatch(tokens, index, allowedObjects);
    }

    private int skipCreateModifiers(List<ScratchBirdV3Token> tokens, int index) {
        boolean changed;
        do {
            changed = false;
            if (hasContext(tokens, index, "OR") && (hasContext(tokens, index + 1, "REPLACE") || hasContext(tokens, index + 1, "ALTER"))) {
                index += 2;
                changed = true;
            }
            if (hasAnyContext(tokens, index, Set.of("UNIQUE", "UNLOGGED", "MATERIALIZED"))) {
                index++;
                changed = true;
            }
            if (hasContext(tokens, index, "GLOBAL") && (hasContext(tokens, index + 1, "TEMPORARY") || hasContext(tokens, index + 1, "TEMP"))) {
                index += 2;
                changed = true;
            } else if (hasAnyContext(tokens, index, Set.of("TEMPORARY", "TEMP"))) {
                index++;
                changed = true;
            }
        } while (changed);
        return index;
    }

    private ObjectMatch bestSurfaceMatch(List<ScratchBirdV3Token> tokens, int index, Set<String> allowedObjects) {
        List<String> words = new ArrayList<>();
        for (int i = index; i < Math.min(tokens.size(), index + 4); i++) {
            if (!tokens.get(i).isIdentifierLike()) {
                break;
            }
            words.add(upper(tokens.get(i)));
            String candidate = String.join(" ", words);
            if (allowedObjects.contains(candidate)) {
                return new ObjectMatch(candidate, i + 1);
            }
        }
        return new ObjectMatch(null, index);
    }

    private void addExpectedObjectDiagnostic(String verb, List<ScratchBirdV3Token> tokens) {
        ScratchBirdV3SourceSpan span = tokens.size() > 1 ? tokens.get(1).span() : tokens.get(0).span();
        diagnostics.add(error(
            "PRS_0504",
            "Expected object type after " + verb,
            "Use a v3 object type such as TABLE, SCHEMA, DOMAIN, JOB, USER, ROLE, CLUSTER, or CUBE",
            span
        ));
    }

    private void validateDelimiterBalance(List<ScratchBirdV3Token> tokens) {
        List<ScratchBirdV3TokenType> stack = new ArrayList<>();
        for (ScratchBirdV3Token token : tokens) {
            switch (token.type()) {
                case LEFT_PAREN, LEFT_BRACKET, LEFT_BRACE -> stack.add(token.type());
                case RIGHT_PAREN -> popDelimiter(stack, ScratchBirdV3TokenType.LEFT_PAREN, token);
                case RIGHT_BRACKET -> popDelimiter(stack, ScratchBirdV3TokenType.LEFT_BRACKET, token);
                case RIGHT_BRACE -> popDelimiter(stack, ScratchBirdV3TokenType.LEFT_BRACE, token);
                case ERROR -> diagnostics.add(error("PRS_JV3_103", "Lexer error token in statement", "", token.span()));
                default -> {
                }
            }
        }
        if (!stack.isEmpty()) {
            ScratchBirdV3Token last = tokens.get(tokens.size() - 1);
            diagnostics.add(error("PRS_JV3_104", "Unclosed delimiter in statement", "Close all parentheses, brackets, and braces", last.span()));
        }
    }

    private void popDelimiter(List<ScratchBirdV3TokenType> stack, ScratchBirdV3TokenType expected, ScratchBirdV3Token token) {
        if (stack.isEmpty() || stack.get(stack.size() - 1) != expected) {
            diagnostics.add(error("PRS_JV3_105", "Mismatched closing delimiter", "", token.span()));
            return;
        }
        stack.remove(stack.size() - 1);
    }

    private ScratchBirdV3Diagnostic removedAliasDiagnostic(ScratchBirdV3Token token) {
        String keyword = upper(token);
        String message = switch (keyword) {
            case "DESC" -> "DESC alias is not supported in v3";
            case "VACUUM" -> "VACUUM is not supported in v3";
            case "WAIT" -> "Top-level WAIT is not supported in v3";
            case "FILTER" -> "FILTER DOC PATH alias is not supported in v3";
            case "AGGREGATE" -> "AGGREGATE TIME BUCKET alias is not supported in v3";
            case "ANN" -> "ANN alias is not supported in v3";
            case "CQL", "MONGO", "CYPHER", "MILVUS" -> "Engine-prefixed NoSQL aliases are not supported in v3";
            case "EVAL", "XGROUP", "XREADGROUP", "XCLAIM" -> "Removed Redis alias surface is not supported in v3";
            default -> keyword + " alias is not supported in v3";
        };
        String hint = switch (keyword) {
            case "DESC" -> "Use DESCRIBE";
            case "VACUUM" -> "Use SWEEP DATABASE";
            case "FILTER" -> "Use DOC PATH FILTER";
            case "AGGREGATE" -> "Use TS BUCKET AGG";
            case "ANN" -> "Use VECTOR ANN QUERY";
            default -> "Use the canonical v3 surface";
        };
        return error("PRS_0505", message, hint, token.span());
    }

    private ScratchBirdV3Statement statement(
        ScratchBirdV3StatementKind kind,
        ScratchBirdV3StatementFamily family,
        String surface,
        List<ScratchBirdV3Token> tokens,
        ScratchBirdV3SourceSpan span,
        boolean complete
    ) {
        return new ScratchBirdV3Statement(kind, family, surface, List.copyOf(tokens), span, complete);
    }

    private ScratchBirdV3Diagnostic error(String code, String message, String hint, ScratchBirdV3SourceSpan span) {
        return new ScratchBirdV3Diagnostic(ScratchBirdV3Diagnostic.Severity.ERROR, code, message, hint, span);
    }

    private ScratchBirdV3SourceSpan span(List<ScratchBirdV3Token> tokens) {
        ScratchBirdV3Token first = tokens.get(0);
        ScratchBirdV3Token last = tokens.get(tokens.size() - 1);
        return new ScratchBirdV3SourceSpan(first.span().start(), last.span().endOffset() - first.span().start().offset());
    }

    private ScratchBirdV3SourceSpan spanForOffset(int offset) {
        return new ScratchBirdV3SourceSpan(new ScratchBirdV3SourceLocation(1, offset + 1, offset), 0);
    }

    private String surface(List<ScratchBirdV3Token> tokens, int maxWords) {
        List<String> words = new ArrayList<>();
        for (ScratchBirdV3Token token : tokens) {
            if (!token.isIdentifierLike()) {
                break;
            }
            words.add(upper(token));
            if (words.size() == maxWords) {
                break;
            }
        }
        return String.join(" ", words);
    }

    private boolean hasContext(List<ScratchBirdV3Token> tokens, int index, String keyword) {
        return index < tokens.size() && upper(tokens.get(index)).equals(keyword);
    }

    private boolean hasAnyContext(List<ScratchBirdV3Token> tokens, int index, Set<String> keywords) {
        return index < tokens.size() && keywords.contains(upper(tokens.get(index)));
    }

    private static String upper(ScratchBirdV3Token token) {
        return token.text().toUpperCase(Locale.ROOT);
    }

    public static List<String> gatekeeperKeywords() {
        List<String> keywords = new ArrayList<>(List.of(
            "SELECT", "INSERT", "UPDATE", "DELETE", "MERGE", "CREATE", "ALTER", "DROP", "TRUNCATE",
            "COPY", "GRANT", "REVOKE", "COMMIT", "ROLLBACK", "BEGIN", "END", "DECLARE", "SET",
            "SHOW", "EXPLAIN", "ANALYZE", "CALL", "EXECUTE", "PREPARE", "FROM", "WHERE", "GROUP",
            "HAVING", "ORDER", "LIMIT", "OFFSET", "UNION", "INTERSECT", "EXCEPT", "WITH", "AND",
            "OR", "NOT", "IS", "IN", "BETWEEN", "LIKE", "DIV", "STARTING", "CONTAINING", "CASE",
            "WHEN", "THEN", "ELSE", "NULL", "TRUE", "FALSE", "EXISTS", "CAST", "AS", "JOIN", "ON",
            "USING", "LATERAL", "VALUES", "INTO", "DEFAULT", "START", "IF", "RETURN"
        ));
        keywords.addAll(ScratchBirdV3StatementCatalog.TOP_LEVEL_CONTEXTUAL.keySet());
        return keywords.stream().distinct().sorted(Comparator.naturalOrder()).toList();
    }

    public static List<String> dialectKeywords() {
        List<String> keywords = new ArrayList<>(gatekeeperKeywords());
        keywords.addAll(ScratchBirdV3StatementCatalog.DIALECT_CONTEXTUAL_KEYWORDS);
        return keywords.stream().distinct().sorted(Comparator.naturalOrder()).toList();
    }

    private static List<ScratchBirdV3Completion> createCompletions(
        List<ScratchBirdV3Token> contextTokens,
        String activePrefix
    ) {
        if (contextTokens.size() <= 1) {
            return objectCompletions(ScratchBirdV3StatementCatalog.CREATE_OBJECTS, "CREATE object type", activePrefix);
        }
        ObjectMatch match = staticBestSurfaceMatch(contextTokens, 1, ScratchBirdV3StatementCatalog.CREATE_OBJECTS);
        if (match.surface == null) {
            return objectCompletions(ScratchBirdV3StatementCatalog.CREATE_OBJECTS, "CREATE object type", activePrefix);
        }
        return switch (match.surface) {
            case "TABLE", "TEMP TABLE", "TEMPORARY TABLE", "GLOBAL TEMPORARY TABLE" -> objectCompletions(
                ScratchBirdV3StatementCatalog.CREATE_TABLE_SUFFIXES,
                "CREATE TABLE clause",
                activePrefix);
            case "VIEW" -> objectCompletions(ScratchBirdV3StatementCatalog.CREATE_VIEW_SUFFIXES, "CREATE VIEW clause", activePrefix);
            case "DOMAIN", "TYPE" -> objectCompletions(ScratchBirdV3StatementCatalog.CREATE_TYPE_SUFFIXES, "Type definition clause", activePrefix);
            case "JOB" -> objectCompletions(ScratchBirdV3StatementCatalog.CREATE_JOB_SUFFIXES, "CREATE JOB clause", activePrefix);
            default -> List.of();
        };
    }

    private static List<ScratchBirdV3Completion> showCompletions(
        List<ScratchBirdV3Token> contextTokens,
        String activePrefix
    ) {
        if (contextTokens.size() <= 1) {
            List<ScratchBirdV3Completion> completions = new ArrayList<>();
            completions.addAll(completionsFrom(ScratchBirdV3StatementCatalog.SHOW_OBJECTS, "SHOW surface"));
            completions.addAll(completionsFrom(ScratchBirdV3StatementCatalog.SHOW_CONTROL_PREFIXES, "SHOW control surface"));
            return filterCompletions(completions, activePrefix);
        }
        String second = upper(contextTokens.get(1));
        if ("MANAGEMENT".equals(second)) {
            return objectCompletions(ScratchBirdV3StatementCatalog.SHOW_MANAGEMENT_SUFFIXES, "SHOW MANAGEMENT surface", activePrefix);
        }
        if ("CLUSTER".equals(second)) {
            return objectCompletions(ScratchBirdV3StatementCatalog.SHOW_CLUSTER_SUFFIXES, "SHOW CLUSTER surface", activePrefix);
        }
        if ("CURRENT".equals(second)) {
            return objectCompletions(Set.of("SCHEMA"), "SHOW CURRENT surface", activePrefix);
        }
        if ("SCHEMA".equals(second)) {
            return objectCompletions(Set.of("PATH"), "SHOW SCHEMA surface", activePrefix);
        }
        if ("SEARCH".equals(second)) {
            return objectCompletions(Set.of("PATH"), "SHOW SEARCH surface", activePrefix);
        }
        if ("SQL".equals(second)) {
            return objectCompletions(Set.of("DIALECT"), "SHOW SQL surface", activePrefix);
        }
        if ("TIME".equals(second)) {
            return objectCompletions(Set.of("ZONE"), "SHOW TIME surface", activePrefix);
        }
        if ("TRANSACTION".equals(second)) {
            return objectCompletions(Set.of("ISOLATION LEVEL"), "SHOW TRANSACTION surface", activePrefix);
        }
        return List.of();
    }

    private static List<ScratchBirdV3Completion> setCompletions(
        List<ScratchBirdV3Token> contextTokens,
        String activePrefix
    ) {
        if (contextTokens.size() <= 1) {
            return objectCompletions(ScratchBirdV3StatementCatalog.SET_PREFIXES, "SET surface", activePrefix);
        }
        String second = upper(contextTokens.get(1));
        if ("SCHEMA".equals(second) || "CURRENT_SCHEMA".equals(second)) {
            return objectCompletions(ScratchBirdV3StatementCatalog.SET_SCHEMA_VALUES, "SET SCHEMA value", activePrefix);
        }
        if ("ROLE".equals(second)) {
            return objectCompletions(ScratchBirdV3StatementCatalog.SET_ROLE_VALUES, "SET ROLE value", activePrefix);
        }
        if ("TIME".equals(second)) {
            return objectCompletions(Set.of("ZONE"), "SET TIME surface", activePrefix);
        }
        if ("TRANSACTION".equals(second)) {
            return objectCompletions(ScratchBirdV3StatementCatalog.SET_TRANSACTION_SUFFIXES, "SET TRANSACTION clause", activePrefix);
        }
        if ("SQL".equals(second)) {
            return objectCompletions(Set.of("DIALECT"), "SET SQL surface", activePrefix);
        }
        if ("PARSER".equals(second)) {
            return objectCompletions(Set.of("VERSION"), "SET PARSER surface", activePrefix);
        }
        return List.of();
    }

    private static List<ScratchBirdV3Completion> objectCompletions(Set<String> values, String detail, String activePrefix) {
        return filterCompletions(completionsFrom(values, detail), activePrefix);
    }

    private static List<ScratchBirdV3Completion> completionsFrom(Set<String> values, String detail) {
        return values.stream()
            .sorted()
            .map(value -> new ScratchBirdV3Completion(value, detail))
            .toList();
    }

    private static List<ScratchBirdV3Completion> filterCompletions(
        List<ScratchBirdV3Completion> completions,
        String activePrefix
    ) {
        if (activePrefix.isBlank()) {
            return completions;
        }
        String prefix = activePrefix.toUpperCase(Locale.ROOT);
        return completions.stream()
            .filter(completion -> completion.label().toUpperCase(Locale.ROOT).startsWith(prefix))
            .toList();
    }

    private static ObjectMatch staticBestSurfaceMatch(
        List<ScratchBirdV3Token> tokens,
        int index,
        Set<String> allowedObjects
    ) {
        List<String> words = new ArrayList<>();
        for (int i = index; i < Math.min(tokens.size(), index + 4); i++) {
            if (!tokens.get(i).isIdentifierLike()) {
                break;
            }
            words.add(upper(tokens.get(i)));
            String candidate = String.join(" ", words);
            if (allowedObjects.contains(candidate)) {
                return new ObjectMatch(candidate, i + 1);
            }
        }
        return new ObjectMatch(null, index);
    }

    private record ObjectMatch(String surface, int nextIndex) {
    }
}
