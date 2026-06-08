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

public enum ScratchBirdV3StatementKind {
    CREATE,
    ALTER,
    DROP,
    TRUNCATE,
    RECREATE,
    DECLARE,

    SELECT,
    INSERT,
    UPDATE,
    UPDATE_OR_INSERT,
    DELETE,
    COPY,
    MERGE,
    WITH,

    BEGIN,
    START,
    PREPARE,
    COMMIT,
    ROLLBACK,
    SAVEPOINT,
    RELEASE_SAVEPOINT,

    USE,
    SET,
    RESET,
    SHOW,
    DESCRIBE,
    CONFIG,

    EXECUTE,
    EXECUTE_BLOCK,
    EXECUTE_JOB,
    CALL,
    PSQL_BLOCK,
    PSQL_IF,
    PSQL_RETURN,

    EXPLAIN,
    ANALYZE,
    SWEEP,
    VALIDATE,
    CANCEL_JOB,
    SECURITY_LABEL,

    GRANT,
    REVOKE,
    CONNECT,
    DISCONNECT,
    COMMENT,

    MANAGEMENT_CONTROL,
    CLUSTER_CONTROL,
    CUBE_CONTROL,
    SERVICE_CHANNEL,
    ADMIN_CONTROL,
    MIGRATION_CONTROL,
    UDR_CONTROL,
    EXTENSION_CONTROL,

    DOC_PATH_FILTER,
    TIME_BUCKET_AGG,
    SEARCH_DSL,
    VECTOR_ANN,
    GRAPH_PATH,
    REDIS,
    HYBRID_BRIDGE,

    UNKNOWN
}
