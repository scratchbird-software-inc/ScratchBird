// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

class SBSQLParserTest {

    @Test
    void convertsStoredProcedureEscapeToCall() {
        String sql = SBSQLParser.convertToNativeSQL("{call demo_proc(?, ?)}");
        assertEquals("CALL demo_proc(?, ?)", sql);
    }

    @Test
    void convertsStoredFunctionEscapeToSelect() {
        String sql = SBSQLParser.convertToNativeSQL("{? = call demo_fn(?)}");
        assertEquals("SELECT demo_fn(?)", sql);
    }

    @Test
    void convertsJdbcFunctionEscapesToCanonicalV3Forms() {
        assertEquals("SELECT LOWER('ABC')", SBSQLParser.convertToNativeSQL("SELECT {fn LCASE('ABC')}"));
        assertEquals("SELECT UPPER('abc')", SBSQLParser.convertToNativeSQL("SELECT {fn UCASE('abc')}"));
        assertEquals("SELECT TRUNC(3.14159, 2)",
                SBSQLParser.convertToNativeSQL("SELECT {fn TRUNCATE(3.14159, 2)}"));
        assertEquals("SELECT SUBSTRING('abcdef', 2, 3)",
                SBSQLParser.convertToNativeSQL("SELECT {fn SUBSTRING('abcdef', 2, 3)}"));
        assertEquals("SELECT CURRENT_DATE", SBSQLParser.convertToNativeSQL("SELECT {fn CURDATE()}"));
        assertEquals("SELECT CURRENT_TIME", SBSQLParser.convertToNativeSQL("SELECT {fn CURTIME()}"));
        assertEquals("SELECT CURRENT_TIMESTAMP", SBSQLParser.convertToNativeSQL("SELECT {fn NOW()}"));
        assertEquals("SELECT EXTRACT(DAY FROM my_date)",
                SBSQLParser.convertToNativeSQL("SELECT {fn DAYOFMONTH(my_date)}"));
        assertEquals("SELECT EXTRACT(MONTH FROM my_date)",
                SBSQLParser.convertToNativeSQL("SELECT {fn MONTH(my_date)}"));
        assertEquals("SELECT EXTRACT(YEAR FROM my_date)",
                SBSQLParser.convertToNativeSQL("SELECT {fn YEAR(my_date)}"));
        assertEquals("SELECT EXTRACT(HOUR FROM my_ts)",
                SBSQLParser.convertToNativeSQL("SELECT {fn HOUR(my_ts)}"));
        assertEquals("SELECT EXTRACT(MINUTE FROM my_ts)",
                SBSQLParser.convertToNativeSQL("SELECT {fn MINUTE(my_ts)}"));
        assertEquals("SELECT EXTRACT(SECOND FROM my_ts)",
                SBSQLParser.convertToNativeSQL("SELECT {fn SECOND(my_ts)}"));
        assertEquals("SELECT CURRENT_USER", SBSQLParser.convertToNativeSQL("SELECT {fn USER()}"));
        assertEquals("SELECT CURRENT_DATABASE()",
                SBSQLParser.convertToNativeSQL("SELECT {fn DATABASE()}"));
    }

    @Test
    void convertsJdbcLiteralAndWrapperEscapesToCanonicalForms() {
        assertEquals("SELECT CAST('2026-03-03' AS DATE)",
                SBSQLParser.convertToNativeSQL("SELECT {d '2026-03-03'}"));
        assertEquals("SELECT CAST('12:34:56' AS TIME)",
                SBSQLParser.convertToNativeSQL("SELECT {t '12:34:56'}"));
        assertEquals("SELECT CAST('2026-03-03 12:34:56' AS TIMESTAMP)",
                SBSQLParser.convertToNativeSQL("SELECT {ts '2026-03-03 12:34:56'}"));
        assertEquals("SELECT * FROM demo WHERE c LIKE 'a!_%' ESCAPE '!'",
                SBSQLParser.convertToNativeSQL("SELECT * FROM demo WHERE c LIKE 'a!_%' {escape '!'}"));
        assertEquals("SELECT * FROM t1 LEFT OUTER JOIN t2 ON t1.id = t2.id",
                SBSQLParser.convertToNativeSQL("SELECT * FROM {oj t1 LEFT OUTER JOIN t2 ON t1.id = t2.id}"));
    }
}
