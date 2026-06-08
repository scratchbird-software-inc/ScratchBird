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

class SBConnectionSchemaStatementTest {

    @Test
    void buildsSchemaStatementForRecursiveSchemaPath() {
        assertEquals("SET SCHEMA \"public\".\"examples\"",
            SBConnection.buildSchemaStatement("public.examples"));
    }

    @Test
    void buildsSearchPathStatementForMultipleRecursiveSchemas() {
        assertEquals("SET SEARCH_PATH TO \"public\".\"examples\", \"compat\".\"mysql\"",
            SBConnection.buildSchemaStatement("public.examples, compat.mysql"));
    }

    @Test
    void preservesQuotedSchemaSegments() {
        assertEquals("SET SCHEMA \"Public\".\"Examples\"",
            SBConnection.buildSchemaStatement("\"Public\".\"Examples\""));
    }
}
