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
import java.util.Map;
import java.util.Set;

final class ScratchBirdV3StatementCatalog {
    static final Set<String> CREATE_OBJECTS = Set.of(
        "ACCESS METHOD",
        "CDC TABLE",
        "CLUSTER",
        "CONNECTION RULE",
        "CUBE",
        "DATABASE",
        "DATABASE CONNECTION",
        "DOMAIN",
        "EXCEPTION",
        "EXTENSION",
        "FOREIGN DATA WRAPPER",
        "FOREIGN TABLE",
        "FUNCTION",
        "GLOBAL TEMPORARY TABLE",
        "GROUP",
        "INDEX",
        "JOB",
        "MIGRATION JOB",
        "PACKAGE",
        "POLICY",
        "PROCEDURE",
        "PUBLIC SYNONYM",
        "PUBLICATION",
        "QUOTA PROFILE",
        "REPLICATION CHANNEL",
        "ROLE",
        "SCHEMA",
        "SEQUENCE",
        "SERVER",
        "STATISTICS",
        "SUBSCRIPTION",
        "SYNONYM",
        "TABLE",
        "TABLESPACE",
        "TEMP TABLE",
        "TEMPORARY TABLE",
        "TOKEN",
        "TRANSFORM",
        "TRIGGER",
        "TYPE",
        "UDR",
        "USER",
        "USER MAPPING",
        "VIEW"
    );

    static final Set<String> ALTER_OBJECTS = Set.of(
        "CDC TABLE",
        "CLUSTER",
        "CONNECTION RULE",
        "CUBE",
        "DATABASE",
        "DATABASE CONNECTION",
        "DOMAIN",
        "EXTENSION",
        "FUNCTION",
        "GROUP",
        "INDEX",
        "JOB",
        "PACKAGE",
        "POLICY",
        "PROCEDURE",
        "PUBLIC SYNONYM",
        "PUBLICATION",
        "QUOTA PROFILE",
        "REPLICATION CHANNEL",
        "ROLE",
        "SCHEMA",
        "SEQUENCE",
        "SERVER",
        "SUBSCRIPTION",
        "SYNONYM",
        "SYSTEM",
        "TABLE",
        "TABLESPACE",
        "TOKEN",
        "TRIGGER",
        "TYPE",
        "USER",
        "USER MAPPING",
        "VIEW"
    );

    static final Set<String> DROP_OBJECTS = Set.of(
        "CDC TABLE",
        "CLUSTER",
        "COMMENT",
        "CONNECTION RULE",
        "CUBE",
        "DATABASE",
        "DATABASE CONNECTION",
        "DOMAIN",
        "EXCEPTION",
        "EXTENSION",
        "FOREIGN TABLE",
        "FUNCTION",
        "GROUP",
        "INDEX",
        "JOB",
        "PACKAGE",
        "POLICY",
        "PROCEDURE",
        "PUBLIC SYNONYM",
        "PUBLICATION",
        "QUOTA PROFILE",
        "REPLICATION CHANNEL",
        "ROLE",
        "SCHEMA",
        "SEQUENCE",
        "SERVER",
        "SUBSCRIPTION",
        "SYNONYM",
        "TABLE",
        "TABLESPACE",
        "TOKEN",
        "TRIGGER",
        "TYPE",
        "UDR",
        "USER",
        "USER MAPPING",
        "VIEW"
    );

    static final Set<String> SHOW_OBJECTS = Set.of(
        "ALL",
        "CHECK",
        "CHECKS",
        "COLLATION",
        "COLLATIONS",
        "COLUMN",
        "COLUMNS",
        "COMMENT",
        "COMMENTS",
        "CREATE TABLE",
        "CURRENT SCHEMA",
        "CURRENT_SCHEMA",
        "DATABASE",
        "DATABASES",
        "DEPENDENCIES",
        "DEPENDENCY",
        "DOMAIN",
        "DOMAINS",
        "FUNCTION",
        "FUNCTIONS",
        "GENERATOR",
        "GENERATORS",
        "GRANTS",
        "INDEX",
        "INDEXES",
        "JOB",
        "JOBS",
        "MIGRATION",
        "METRICS",
        "PACKAGE",
        "PACKAGES",
        "PARSER VERSION",
        "PROCEDURE",
        "PROCEDURES",
        "ROLE",
        "ROLES",
        "SCHEMA",
        "SCHEMA PATH",
        "SCHEMAS",
        "SEARCH PATH",
        "SEARCH_PATH",
        "SEQUENCE",
        "SEQUENCES",
        "SQL DIALECT",
        "SYSTEM",
        "TABLE",
        "TABLES",
        "TIME ZONE",
        "TRANSACTION ISOLATION LEVEL",
        "TRIGGER",
        "TRIGGERS",
        "VERSION",
        "VIEW",
        "VIEWS"
    );

    static final Set<String> SHOW_CONTROL_PREFIXES = Set.of(
        "ADMISSION",
        "ALERT",
        "AUTOSCALE",
        "CLUSTER",
        "CUBE",
        "ERROR",
        "MANAGEMENT",
        "READINESS",
        "SLO",
        "SUPPORT"
    );

    static final Set<String> SET_PREFIXES = Set.of(
        "AUTOCOMMIT",
        "CONCURRENCY MODE",
        "CONSISTENCY",
        "CONSTRAINTS",
        "LOCAL_TIMEOUT",
        "NAMES",
        "PARSER",
        "ROLE",
        "SCHEMA",
        "SEARCH_PATH",
        "SEQUENCE",
        "SERIAL CONSISTENCY",
        "SESSION AUTHORIZATION",
        "SINGLE_WRITER",
        "SQL DIALECT",
        "TERM",
        "TIME ZONE",
        "TRANSACTION"
    );

    static final Set<String> RESET_PREFIXES = Set.of(
        "ALL",
        "ROLE",
        "SCHEMA",
        "SEARCH_PATH",
        "SESSION AUTHORIZATION",
        "TIME ZONE"
    );

    static final Set<String> EXECUTE_PREFIXES = Set.of(
        "BLOCK",
        "JOB",
        "PROCEDURE",
        "STATEMENT"
    );

    static final Set<String> DESCRIBE_OBJECTS = Set.of(
        "DOMAIN",
        "FUNCTION",
        "INDEX",
        "JOB",
        "PACKAGE",
        "PROCEDURE",
        "SCHEMA",
        "SEQUENCE",
        "TABLE",
        "TRIGGER",
        "TYPE",
        "VIEW"
    );

    static final Set<String> USE_PREFIXES = Set.of(
        "DATABASE",
        "SCHEMA",
        "SNAPSHOT"
    );

    static final Set<String> SET_SCHEMA_VALUES = Set.of(
        "DEFAULT",
        "sys",
        "users.public",
        "data"
    );

    static final Set<String> SET_ROLE_VALUES = Set.of(
        "DEFAULT",
        "NONE"
    );

    static final Set<String> SET_TRANSACTION_SUFFIXES = Set.of(
        "READ ONLY",
        "READ WRITE",
        "ISOLATION LEVEL",
        "SNAPSHOT"
    );

    static final Set<String> CREATE_TABLE_SUFFIXES = Set.of(
        "AS SELECT",
        "LIKE",
        "PARTITION BY",
        "PRIMARY KEY",
        "USING",
        "WITH"
    );

    static final Set<String> CREATE_VIEW_SUFFIXES = Set.of(
        "AS SELECT",
        "WITH CHECK OPTION",
        "WITH CASCADED CHECK OPTION",
        "WITH LOCAL CHECK OPTION"
    );

    static final Set<String> CREATE_TYPE_SUFFIXES = Set.of(
        "AS",
        "CHECK",
        "COLLATE",
        "DEFAULT",
        "NOT NULL"
    );

    static final Set<String> CREATE_JOB_SUFFIXES = Set.of(
        "AS",
        "ON SCHEDULE",
        "OWNED BY",
        "WITH"
    );

    static final Set<String> SHOW_MANAGEMENT_SUFFIXES = Set.of(
        "DRIFT",
        "INSTRUCTIONS",
        "LISTENERS",
        "MANAGER",
        "PARSER POOL",
        "SERVERS"
    );

    static final Set<String> SHOW_CLUSTER_SUFFIXES = Set.of(
        "ADMISSION STATUS",
        "PROVIDER",
        "ROUTING PLAN",
        "STATE ADMISSION_TUNING_HISTORY",
        "STATE ALERT_DASHBOARD",
        "STATE AUTOSCALE_ACTIONS",
        "STATE ERROR_BUDGET_STATUS",
        "STATE READINESS_HEALTH",
        "STATE SLO_STATUS",
        "STATE SUPPORT_BUNDLE_SAFETY"
    );

    static final Set<String> DIALECT_CONTEXTUAL_KEYWORDS = Set.of(
        "ERROR",
        "ERRORS",
        "GLOBAL",
        "METRICS",
        "SESSION",
        "STATUS"
    );

    static final Map<String, ScratchBirdV3StatementKind> TOP_LEVEL_CONTEXTUAL = Map.ofEntries(
        Map.entry("BACKUP", ScratchBirdV3StatementKind.ADMIN_CONTROL),
        Map.entry("CHECKPOINT", ScratchBirdV3StatementKind.ADMIN_CONTROL),
        Map.entry("CLUSTER", ScratchBirdV3StatementKind.CLUSTER_CONTROL),
        Map.entry("COMMENT", ScratchBirdV3StatementKind.COMMENT),
        Map.entry("CONFIG", ScratchBirdV3StatementKind.CONFIG),
        Map.entry("CONNECT", ScratchBirdV3StatementKind.CONNECT),
        Map.entry("CUBE", ScratchBirdV3StatementKind.CUBE_CONTROL),
        Map.entry("DESCRIBE", ScratchBirdV3StatementKind.DESCRIBE),
        Map.entry("DISCONNECT", ScratchBirdV3StatementKind.DISCONNECT),
        Map.entry("DOC", ScratchBirdV3StatementKind.DOC_PATH_FILTER),
        Map.entry("DRAIN", ScratchBirdV3StatementKind.MANAGEMENT_CONTROL),
        Map.entry("GRAPH", ScratchBirdV3StatementKind.GRAPH_PATH),
        Map.entry("HYBRID", ScratchBirdV3StatementKind.HYBRID_BRIDGE),
        Map.entry("INSTALL", ScratchBirdV3StatementKind.EXTENSION_CONTROL),
        Map.entry("LOAD", ScratchBirdV3StatementKind.EXTENSION_CONTROL),
        Map.entry("MATCH", ScratchBirdV3StatementKind.GRAPH_PATH),
        Map.entry("REDIS", ScratchBirdV3StatementKind.REDIS),
        Map.entry("RECREATE", ScratchBirdV3StatementKind.RECREATE),
        Map.entry("REFRESH", ScratchBirdV3StatementKind.CUBE_CONTROL),
        Map.entry("RESET", ScratchBirdV3StatementKind.RESET),
        Map.entry("RESYNC", ScratchBirdV3StatementKind.MIGRATION_CONTROL),
        Map.entry("RESTORE", ScratchBirdV3StatementKind.ADMIN_CONTROL),
        Map.entry("RESTART", ScratchBirdV3StatementKind.MANAGEMENT_CONTROL),
        Map.entry("SAVEPOINT", ScratchBirdV3StatementKind.SAVEPOINT),
        Map.entry("SEARCH", ScratchBirdV3StatementKind.SEARCH_DSL),
        Map.entry("SECURITY", ScratchBirdV3StatementKind.SECURITY_LABEL),
        Map.entry("SERVICE", ScratchBirdV3StatementKind.SERVICE_CHANNEL),
        Map.entry("STOP", ScratchBirdV3StatementKind.MANAGEMENT_CONTROL),
        Map.entry("SWEEP", ScratchBirdV3StatementKind.SWEEP),
        Map.entry("TS", ScratchBirdV3StatementKind.TIME_BUCKET_AGG),
        Map.entry("UDR", ScratchBirdV3StatementKind.UDR_CONTROL),
        Map.entry("UNDRAIN", ScratchBirdV3StatementKind.MANAGEMENT_CONTROL),
        Map.entry("USE", ScratchBirdV3StatementKind.USE),
        Map.entry("VALIDATE", ScratchBirdV3StatementKind.VALIDATE),
        Map.entry("VECTOR", ScratchBirdV3StatementKind.VECTOR_ANN)
    );

    static final Set<String> REMOVED_ALIASES = Set.of(
        "AGGREGATE",
        "ANN",
        "CQL",
        "CYPHER",
        "DESC",
        "EVAL",
        "FILTER",
        "MILVUS",
        "MONGO",
        "VACUUM",
        "WAIT",
        "XCLAIM",
        "XGROUP",
        "XREADGROUP"
    );

    private ScratchBirdV3StatementCatalog() {
    }

    static List<ScratchBirdV3Completion> statementCompletions() {
        List<ScratchBirdV3Completion> completions = new ArrayList<>();
        completions.add(new ScratchBirdV3Completion("SELECT", "DML query"));
        completions.add(new ScratchBirdV3Completion("WITH", "DML common table expression"));
        completions.add(new ScratchBirdV3Completion("INSERT", "DML insert"));
        completions.add(new ScratchBirdV3Completion("UPDATE", "DML update"));
        completions.add(new ScratchBirdV3Completion("DELETE", "DML delete"));
        completions.add(new ScratchBirdV3Completion("MERGE", "DML merge"));
        completions.add(new ScratchBirdV3Completion("COPY", "Bulk transfer"));
        completions.add(new ScratchBirdV3Completion("CREATE", "DDL create"));
        completions.add(new ScratchBirdV3Completion("ALTER", "DDL alter"));
        completions.add(new ScratchBirdV3Completion("DROP", "DDL drop"));
        completions.add(new ScratchBirdV3Completion("TRUNCATE", "DDL truncate"));
        completions.add(new ScratchBirdV3Completion("SET", "Session setting"));
        completions.add(new ScratchBirdV3Completion("SHOW", "Metadata/session report"));
        completions.add(new ScratchBirdV3Completion("RESET", "Reset session setting"));
        completions.add(new ScratchBirdV3Completion("USE", "Set current schema or snapshot scope"));
        completions.add(new ScratchBirdV3Completion("DESCRIBE", "Describe SQL object"));
        completions.add(new ScratchBirdV3Completion("EXECUTE", "Execute block/procedure/job/statement"));
        completions.add(new ScratchBirdV3Completion("CALL", "Call procedure"));
        completions.add(new ScratchBirdV3Completion("GRANT", "DCL grant"));
        completions.add(new ScratchBirdV3Completion("REVOKE", "DCL revoke"));
        completions.add(new ScratchBirdV3Completion("CONNECT", "Connection command"));
        completions.add(new ScratchBirdV3Completion("DISCONNECT", "Connection command"));
        completions.add(new ScratchBirdV3Completion("CLUSTER", "Cluster control"));
        completions.add(new ScratchBirdV3Completion("SHOW MANAGEMENT", "Management report"));
        completions.add(new ScratchBirdV3Completion("SHOW CLUSTER", "Cluster report"));
        return completions;
    }
}
