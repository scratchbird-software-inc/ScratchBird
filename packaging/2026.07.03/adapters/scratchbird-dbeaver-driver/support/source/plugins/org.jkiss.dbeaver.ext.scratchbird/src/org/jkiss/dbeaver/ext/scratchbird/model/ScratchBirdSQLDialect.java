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
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package org.jkiss.dbeaver.ext.scratchbird.model;

import org.jkiss.code.NotNull;
import org.jkiss.dbeaver.ext.generic.model.GenericSQLDialect;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3Parser;
import org.jkiss.dbeaver.model.DBPKeywordType;
import org.jkiss.dbeaver.model.data.DBDBinaryFormatter;
import org.jkiss.dbeaver.model.exec.jdbc.JDBCDatabaseMetaData;
import org.jkiss.dbeaver.model.exec.jdbc.JDBCSession;
import org.jkiss.dbeaver.model.impl.data.formatters.BinaryFormatterHexString;
import org.jkiss.dbeaver.model.impl.jdbc.JDBCDataSource;

import java.util.Arrays;

public class ScratchBirdSQLDialect extends GenericSQLDialect {

    private static final String[] DDL_KEYWORDS = new String[] {
        "CREATE",
        "RECREATE",
        "ALTER",
        "DROP",
        "TRUNCATE",
        "COMMENT",
        "GRANT",
        "REVOKE"
    };

    private static final String[] EXEC_KEYWORDS = new String[] {
        "CALL",
        "ANALYZE",
        "BACKUP",
        "CHECKPOINT",
        "CLUSTER",
        "CONFIG",
        "CONNECT",
        "COPY",
        "CUBE",
        "DESCRIBE",
        "DISCONNECT",
        "DOC",
        "DRAIN",
        "EXECUTE",
        "EXPLAIN",
        "GRAPH",
        "HYBRID",
        "INSTALL",
        "LOAD",
        "MATCH",
        "REDIS",
        "REFRESH",
        "RESET",
        "RESYNC",
        "RESTORE",
        "RESTART",
        "SAVEPOINT",
        "SEARCH",
        "SECURITY",
        "SERVICE",
        "SET",
        "SHOW",
        "START",
        "STOP",
        "SWEEP",
        "TS",
        "UDR",
        "UNDRAIN",
        "USE",
        "VALIDATE",
        "VECTOR"
    };

    private static final String[] SCRATCHBIRD_FUNCTIONS = new String[] {
        "CURRENT_DATABASE",
        "CURRENT_SCHEMA",
        "CURRENT_ROLE",
        "CURRENT_USER"
    };

    private static final String[] SCRATCHBIRD_TYPES = new String[] {
        "ARRAY",
        "BIGINT",
        "BLOB",
        "BOOLEAN",
        "BYTEA",
        "CHAR",
        "CIDR",
        "COMPOSITE",
        "DATE",
        "DECIMAL",
        "DOUBLE",
        "GEOMETRY",
        "INET",
        "INTEGER",
        "INTERVAL",
        "JSON",
        "JSONB",
        "LINESTRING",
        "MACADDR",
        "MONEY",
        "NUMERIC",
        "POINT",
        "POLYGON",
        "RANGE",
        "REAL",
        "SMALLINT",
        "TEXT",
        "TIME",
        "TIMESTAMP",
        "TIMESTAMPTZ",
        "TSQUERY",
        "TSVECTOR",
        "UUID",
        "UUID_V7",
        "VARIANT",
        "VARCHAR",
        "VECTOR",
        "XML"
    };

    public ScratchBirdSQLDialect() {
        super("ScratchBird", "scratchbird");
        addKeywords(ScratchBirdV3Parser.dialectKeywords(), DBPKeywordType.KEYWORD);
        addDataTypes(Arrays.asList(SCRATCHBIRD_TYPES));
        addFunctions(Arrays.asList(SCRATCHBIRD_FUNCTIONS));
    }

    public void initDriverSettings(JDBCSession session, JDBCDataSource dataSource, JDBCDatabaseMetaData metaData) {
        super.initDriverSettings(session, dataSource, metaData);
        addKeywords(ScratchBirdV3Parser.dialectKeywords(), DBPKeywordType.KEYWORD);
        addDataTypes(Arrays.asList(SCRATCHBIRD_TYPES));
        addFunctions(Arrays.asList(SCRATCHBIRD_FUNCTIONS));
    }

    @Override
    public boolean isStandardSQL() {
        return false;
    }

    @NotNull
    @Override
    public String[] getDDLKeywords() {
        return DDL_KEYWORDS;
    }

    @NotNull
    @Override
    public String[] getExecuteKeywords() {
        return EXEC_KEYWORDS;
    }

    @Override
    public boolean supportsAliasInSelect() {
        return true;
    }

    @Override
    public boolean supportsAliasInHaving() {
        return true;
    }

    @Override
    public boolean supportsAsKeywordBeforeAliasInFromClause() {
        return true;
    }

    @Override
    public boolean supportsCommentQuery() {
        return true;
    }

    @Override
    public boolean supportsInsertAllDefaultValuesStatement() {
        return true;
    }

    @Override
    public boolean supportsIndexCreateAndDrop() {
        return true;
    }

    @Override
    public boolean validIdentifierPart(char c, boolean quoted) {
        return super.validIdentifierPart(c, quoted) || c == '$';
    }

    @NotNull
    @Override
    public DBDBinaryFormatter getNativeBinaryFormatter() {
        return BinaryFormatterHexString.INSTANCE;
    }
}
